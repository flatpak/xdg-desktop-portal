/*
 * Copyright © 2025 Isaiah Inuwa <isaiah.inuwa@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Isaiah Inuwa <isaiah.inuwa@gmail.com>
 */


#include <gio/gunixfdlist.h>
#include <stdint.h>

#include "gio/gio.h"
#include "glib.h"
#include "glibconfig.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-experimental-dbus.h"
#include "xdp-experimental-handler-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-utils.h"

#include "credential.h"

typedef struct _Credential Credential;
typedef struct _CredentialClass CredentialClass;

struct _Credential
{
  XdpDbusExperimentalCredentialSkeleton parent_instance;

  XdpDbusExperimentalHandlerCredential *handler;
};

struct _CredentialClass
{
  XdpDbusExperimentalCredentialSkeletonClass parent_class;
};

GType credential_get_type (void) G_GNUC_CONST;
static void credential_iface_init (XdpDbusExperimentalCredentialIface *iface);

G_DEFINE_TYPE_WITH_CODE (Credential,
                         credential,
                         XDP_DBUS_EXPERIMENTAL_TYPE_CREDENTIAL_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_EXPERIMENTAL_TYPE_CREDENTIAL,
                                                credential_iface_init))


G_DEFINE_AUTOPTR_CLEANUP_FUNC (Credential, g_object_unref)

static XdpOptionKey create_credential_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "origin", G_VARIANT_TYPE_STRING, NULL },
  { "top_origin", G_VARIANT_TYPE_STRING, NULL },
  { "type", G_VARIANT_TYPE_STRING, NULL },
  { "public_key", G_VARIANT_TYPE_STRING, NULL },
};

static XdpOptionKey get_credential_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "origin", G_VARIANT_TYPE_STRING, NULL },
  { "top_origin", G_VARIANT_TYPE_STRING, NULL },
  { "public_key", G_VARIANT_TYPE_STRING, NULL },
};

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  XdpRequest *request = task_data;
  XdgDesktopPortalResponseEnum response;
  GVariant *results = NULL;

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  results = (GVariant *)g_object_get_data (G_OBJECT (request), "results");
  if (!results) {
    g_auto(GVariantBuilder) tmp =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
    results = g_variant_builder_end(&tmp);
  }

  if (request->exported)
    {
      g_debug ("sending response: %d", response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      results);
      xdp_request_unexport (request);
    }
}

static void
create_credential_done (GObject *source,
          GAsyncResult *result,
          gpointer data)
{
  g_autoptr(XdpRequest) request = data;
  XdgDesktopPortalResponseEnum response;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_experimental_handler_credential_call_create_credential_finish (XDP_DBUS_EXPERIMENTAL_HANDLER_CREDENTIAL (source),
                                                         &response,
                                                         &results,
                                                         result,
                                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Handler call failed: %s (%d)", error->message, error->code);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (results)
    g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
handle_create_credential(XdpDbusExperimentalCredential *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_parent_window,
                        const char *arg_type,
                        GVariant *arg_options
)
{
  Credential *credential = (Credential *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) handler_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_auto(GVariantBuilder) caller_info =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *app_display_name = xdp_app_info_get_app_display_name(request->app_info);
  if (!app_display_name)
    app_display_name = "";
  g_variant_builder_add(&caller_info, "{sv}", "app_display_name", g_variant_new_string(app_display_name));

  REQUEST_AUTOLOCK (request);

  handler_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (credential->handler)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (credential->handler)),
    request->id,
    NULL, &error);

  if (!handler_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options,
                           create_credential_options, G_N_ELEMENTS(create_credential_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, handler_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_experimental_credential_complete_create_credential (object, invocation, request->id);

  xdp_dbus_experimental_handler_credential_call_create_credential (credential->handler,
                                             arg_parent_window,
                                             app_id,
                                             app_display_name,
                                             arg_type,
                                             g_variant_builder_end (&options),
                                             NULL,
                                             create_credential_done,
                                             g_object_ref (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
get_credential_done (GObject *source,
          GAsyncResult *result,
          gpointer data)
{
  g_autoptr(XdpRequest) request = data;
  XdgDesktopPortalResponseEnum response;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_experimental_handler_credential_call_get_credential_finish (XDP_DBUS_EXPERIMENTAL_HANDLER_CREDENTIAL (source),
                                                         &response,
                                                         &results,
                                                         result,
                                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s (%d)", error->message, error->code);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (results)
    g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}


static gboolean
handle_get_credential(XdpDbusExperimentalCredential *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_parent_window,
                      GVariant *arg_options)
{
  Credential *credential = (Credential *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) handler_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_auto(GVariantBuilder) caller_info =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *app_display_name = xdp_app_info_get_app_display_name(request->app_info);
  if (!app_display_name)
    app_display_name = "";
  g_variant_builder_add(&caller_info, "{sv}", "app_display_name", g_variant_new_string(app_display_name));

  REQUEST_AUTOLOCK (request);

  handler_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (credential->handler)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (credential->handler)),
    request->id,
    NULL, &error);

  if (!handler_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options,
                           get_credential_options, G_N_ELEMENTS(get_credential_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }


  xdp_request_set_impl_request (request, handler_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_experimental_credential_complete_get_credential (object, invocation, request->id);

  xdp_dbus_experimental_handler_credential_call_get_credential (credential->handler,
                                             arg_parent_window,
                                             app_id,
                                             app_display_name,
                                             g_variant_builder_end (&options),
                                             NULL,
                                             get_credential_done,
                                             g_object_ref (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
credential_iface_init (XdpDbusExperimentalCredentialIface *iface)
{
  iface->handle_create_credential = handle_create_credential;
  iface->handle_get_credential = handle_get_credential;
}

static void
credential_dispose (GObject *object)
{
  Credential *credential = (Credential *) object;

  g_clear_object (&credential->handler);

  G_OBJECT_CLASS (credential_parent_class)->dispose (object);
}

static void
credential_init (Credential *credential)
{
}

static void
credential_class_init (CredentialClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = credential_dispose;
}

static Credential *
credential_new (XdpDbusExperimentalHandlerCredential *handler)
{
  Credential *credential;

  credential = g_object_new (credential_get_type (), NULL);
  credential->handler = g_object_ref (handler);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (credential->handler), G_MAXINT);

  xdp_dbus_experimental_credential_set_conditional_create(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_conditional_get(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_hybrid_transport(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), TRUE);
  xdp_dbus_experimental_credential_set_passkey_platform_authenticator(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), TRUE);
  xdp_dbus_experimental_credential_set_user_verifying_platform_authenticator(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_related_origins(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_signal_all_accepted_credentials(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_signal_current_user_details(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_signal_unknown_credential(XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);

  xdp_dbus_experimental_credential_set_version (XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), 1);

  return credential;
}

static void
proxy_created_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_debug("finishing Credential initialization");
  XdpContext *context = (XdpContext *)user_data;
  g_autoptr(XdpDbusExperimentalHandlerCredential) impl = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(Credential) credential = NULL;

  impl = xdp_dbus_experimental_handler_credential_proxy_new_finish (result, &error);
  if (!impl)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to create credential proxy: %s", error->message);
      return;
    }

  context = XDP_CONTEXT (user_data);

  credential = credential_new (impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&credential)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}

void
init_credential (XdpContext *context,
                  GCancellable *cancellable)
{
  g_info("Initializing Credential Portal");
  g_autoptr(Credential) credential = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusExperimentalHandlerCredential) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, CREDENTIAL_DBUS_IMPL_IFACE);

  if (impl_config == NULL) {
    g_debug("impl_config is NULL");
    return;
  }

  xdp_dbus_experimental_handler_credential_proxy_new (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 impl_config->dbus_name,
                                                 DESKTOP_DBUS_PATH,
                                                 cancellable,
                                                 proxy_created_cb,
                                                 context);
}