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
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-utils.h"

#include "credentials.h"

typedef struct _CredentialsX CredentialsX;
typedef struct _CredentialsXClass CredentialsXClass;

struct _CredentialsX
{
  XdpDbusCredentialsXSkeleton parent_instance;

  XdpDbusImplCredentialsX *impl;
};

struct _CredentialsXClass
{
  XdpDbusCredentialsXSkeletonClass parent_class;
};

GType credentials_x_get_type (void) G_GNUC_CONST;
static void credentials_x_iface_init (XdpDbusCredentialsXIface *iface);

G_DEFINE_TYPE_WITH_CODE (CredentialsX,
                         credentials_x,
                         XDP_DBUS_TYPE_CREDENTIALS_X_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_CREDENTIALS_X,
                                                credentials_x_iface_init))


G_DEFINE_AUTOPTR_CLEANUP_FUNC (CredentialsX, g_object_unref)

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
  if (response == XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
  {
    results = (GVariant *)g_object_get_data (G_OBJECT (request), "results");
  }
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

  if (!xdp_dbus_impl_credentials_x_call_create_credential_finish (XDP_DBUS_IMPL_CREDENTIALS_X (source),
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
handle_create_credential(XdpDbusCredentialsX *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_parent_window,
                        const char* arg_origin,
                        const char* arg_top_origin,
                        GVariant *arg_request,
                        GVariant *arg_options
)
{
  CredentialsX *credentials = (CredentialsX *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_auto(GVariantBuilder) caller_info =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *app_display_name = xdp_app_info_get_app_display_name(request->app_info);
  if (!app_display_name)
    app_display_name = "";
  g_variant_builder_add(&caller_info, "{sv}", "app_display_name", g_variant_new_string(app_display_name));

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (credentials->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (credentials->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options,
                           NULL, 0,
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_credentials_x_complete_create_credential (object, invocation, request->id);

  xdp_dbus_impl_credentials_x_call_create_credential (credentials->impl,
                                             request->id,
                                             app_id,
                                             app_display_name,
                                             arg_parent_window,
                                             arg_origin,
                                             arg_top_origin,
                                             arg_request,
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

  if (!xdp_dbus_impl_credentials_x_call_get_credential_finish (XDP_DBUS_IMPL_CREDENTIALS_X (source),
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
handle_get_credential(XdpDbusCredentialsX *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_parent_window,
                      const char* arg_origin,
                      const char* arg_top_origin,
                      GVariant *arg_request,
                      GVariant *arg_options)
{
  CredentialsX *credentials = (CredentialsX *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_auto(GVariantBuilder) caller_info =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *app_display_name = xdp_app_info_get_app_display_name(request->app_info);
  if (!app_display_name)
    app_display_name = "";
  g_variant_builder_add(&caller_info, "{sv}", "app_display_name", g_variant_new_string(app_display_name));

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (credentials->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (credentials->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options,
                           NULL, 0,
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }


  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_credentials_x_complete_get_credential (object, invocation, request->id);

  xdp_dbus_impl_credentials_x_call_get_credential (credentials->impl,
                                             request->id,
                                             app_id,
                                             app_display_name,
                                             arg_parent_window,
                                             arg_origin,
                                             arg_top_origin,
                                             arg_request,
                                             g_variant_builder_end (&options),
                                             NULL,
                                             get_credential_done,
                                             g_object_ref (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
retrieve_client_capabilities_done (GObject *source,
          GAsyncResult *result,
          gpointer data)
{
  g_autoptr(GDBusMethodInvocation) invocation = data;
  g_autoptr(GVariant) capabilities = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_credentials_x_call_get_client_capabilities_finish (XDP_DBUS_IMPL_CREDENTIALS_X (source),
                                                         &capabilities,
                                                         result,
                                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  xdp_dbus_credentials_x_complete_get_client_capabilities (NULL, invocation, capabilities);
}

static gboolean
handle_get_client_capabilities(XdpDbusCredentialsX *object,
                               GDBusMethodInvocation *invocation)
{
  CredentialsX *credentials = (CredentialsX *) object;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (credentials->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (credentials->impl)),
    NULL,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_credentials_x_call_get_client_capabilities (credentials->impl,
                                             NULL,
                                             retrieve_client_capabilities_done,
                                             g_object_ref(invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
credentials_x_iface_init (XdpDbusCredentialsXIface *iface)
{
  iface->handle_create_credential = handle_create_credential;
  iface->handle_get_credential = handle_get_credential;
  iface->handle_get_client_capabilities = handle_get_client_capabilities;
}

static void
credentials_x_dispose (GObject *object)
{
  CredentialsX *credentials_x = (CredentialsX *) object;

  g_clear_object (&credentials_x->impl);

  G_OBJECT_CLASS (credentials_x_parent_class)->dispose (object);
}

static void
credentials_x_init (CredentialsX *clipboard)
{
}

static void
credentials_x_class_init (CredentialsXClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = credentials_x_dispose;
}

static CredentialsX *
credentials_new (XdpDbusImplCredentialsX *impl)
{
  CredentialsX *credentials;

  credentials = g_object_new (credentials_x_get_type (), NULL);
  credentials->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (credentials->impl), G_MAXINT);

  xdp_dbus_credentials_x_set_version (XDP_DBUS_CREDENTIALS_X (credentials), 1);

  /*
  g_signal_connect_object (credentials->impl, "selection-transfer",
                           G_CALLBACK (selection_transfer_cb),
                           impl,
                           G_CONNECT_DEFAULT);

  g_signal_connect_object (credentials->impl, "selection-owner-changed",
                           G_CALLBACK (selection_owner_changed_cb),
                           impl,
                           G_CONNECT_DEFAULT);
    */

  return credentials;
}

static void
proxy_created_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_debug("finishing CredentialsX initialization");
  XdpContext *context = (XdpContext *)user_data;
  g_autoptr(XdpDbusImplCredentialsX) impl = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(CredentialsX) credentials = NULL;

  impl = xdp_dbus_impl_credentials_x_proxy_new_finish (result, &error);
  if (!impl)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to create credentials proxy: %s", error->message);
      return;
    }

  context = XDP_CONTEXT (user_data);

  credentials = credentials_new (impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&credentials)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}

void
init_credentials (XdpContext *context,
                  GCancellable *cancellable)
{
  g_info("Initializing Credentials Portal");
  g_autoptr(CredentialsX) credentials = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplCredentialsX) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, CREDENTIALS_DBUS_IMPL_IFACE);

  if (impl_config == NULL) {
    g_debug("impl_config is NULL");
    return;
  }

  xdp_dbus_impl_credentials_x_proxy_new (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 impl_config->dbus_name,
                                                 DESKTOP_DBUS_PATH,
                                                 cancellable,
                                                 proxy_created_cb,
                                                 context);
}