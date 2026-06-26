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

#include "credential.h"

#include <stdint.h>

#include <gio/gunixfdlist.h>

#include "dex-aio.h"
#include "gio/gio.h"
#include "glib.h"
#include "glibconfig.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-experimental-dbus.h"
#include "xdp-experimental-handler-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request-dex.h"
#include "xdp-utils.h"

static gboolean handle_create_credential (XdpDbusExperimentalCredential *object,
                                          GDBusMethodInvocation *invocation,
                                          const gchar *arg_parent_window,
                                          const gchar *arg_origin,
                                          const gchar *arg_type,
                                          GVariant *arg_options);

static gboolean handle_get_credential (XdpDbusExperimentalCredential *object,
                                       GDBusMethodInvocation *invocation,
                                       const gchar *arg_parent_window,
                                       const gchar *arg_origin,
                                       GVariant *arg_options);
struct _XdpCredential
{
  XdpDbusExperimentalCredentialSkeleton parent_instance;

  XdpContext *context;
  XdpDbusExperimentalHandlerCredential *handler;
};

#define XDP_TYPE_CREDENTIAL (xdp_dbus_experimental_credential_get_type ())
G_DECLARE_FINAL_TYPE (XdpCredential, xdp_credential, XDP, CREDENTIAL, XdpDbusExperimentalCredentialSkeleton)

static void
xdp_credential_iface_init (XdpDbusExperimentalCredentialIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
    XdpCredential,
    xdp_credential,
    XDP_DBUS_EXPERIMENTAL_TYPE_CREDENTIAL_SKELETON,
    G_IMPLEMENT_INTERFACE (XDP_DBUS_EXPERIMENTAL_TYPE_CREDENTIAL, xdp_credential_iface_init)
  );

static void xdp_credential_iface_init (XdpDbusExperimentalCredentialIface *iface)
{
  iface->handle_create_credential = handle_create_credential;
  iface->handle_get_credential = handle_get_credential;
}

static void xdp_credential_dispose (GObject *object)
{
  XdpCredential *credential = XDP_CREDENTIAL (object);

  g_clear_object (&credential->handler);

  G_OBJECT_CLASS (xdp_credential_parent_class)->dispose (object);
}

static void xdp_credential_init (XdpCredential *credential) {}

static void xdp_credential_class_init (XdpCredentialClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_credential_dispose;
}

static XdpCredential *
xdp_credential_new(XdpContext *context, XdpDbusExperimentalHandlerCredential *handler)
{
  XdpCredential *credential;

  credential = g_object_new(xdp_credential_get_type(), NULL);
  credential->context = context;
  credential->handler = g_object_ref(handler);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (credential->handler), G_MAXINT);

  xdp_dbus_experimental_credential_set_conditional_create (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_conditional_get (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_hybrid_transport (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), TRUE);
  xdp_dbus_experimental_credential_set_passkey_platform_authenticator (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), TRUE);
  xdp_dbus_experimental_credential_set_user_verifying_platform_authenticator (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_related_origins (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), TRUE);
  xdp_dbus_experimental_credential_set_signal_all_accepted_credentials (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_signal_current_user_details (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);
  xdp_dbus_experimental_credential_set_signal_unknown_credential (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), FALSE);

  xdp_dbus_experimental_credential_set_version (
      XDP_DBUS_EXPERIMENTAL_CREDENTIAL (credential), 1);

  return credential;
}

const gchar *CREDENTIALSD_HANDLER_DBUS_NAME =
    "xyz.iinuwa.credentialsd.Credentials";

static XdpOptionKey create_credential_options[] = {
    {"handle_token", G_VARIANT_TYPE_STRING, NULL},
    {"origin", G_VARIANT_TYPE_STRING, NULL},
    {"top_origin", G_VARIANT_TYPE_STRING, NULL},
    {"type", G_VARIANT_TYPE_STRING, NULL},
    {"public_key", G_VARIANT_TYPE_STRING, NULL},
};

/**
 * create_credential_validate_options:
 * @arg_options: (transfer none): options passed to the frontend.
 * @error: (transfer none): pointer to an error pointer that will be populated on error.
 * Returns: (transfer full): Filtered list of options to pass to the handler.
 */
static GVariant *
create_credential_validate_options (GVariant *arg_options,
                                 GError  **error)
{
  g_auto (GVariantBuilder) options = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  gboolean validated = xdp_filter_options (arg_options,
                                           &options,
                                           create_credential_options,
                                           G_N_ELEMENTS (create_credential_options),
                                           NULL,
                                           error);
  if (!validated)
      return NULL;
  else
      return g_variant_ref_sink (g_variant_builder_end (&options));
}

static gboolean handle_create_credential (XdpDbusExperimentalCredential *object,
                                          GDBusMethodInvocation         *invocation,
                                          const gchar                   *arg_parent_window,
                                          const gchar                   *arg_origin,
                                          const gchar                   *arg_type,
                                          GVariant                      *arg_options)
{
  XdpCredential *credential = XDP_CREDENTIAL (object);
  g_autoptr (XdpRequestDex) request = NULL;
  g_autoptr (GVariant) options = NULL;
  g_autoptr (GError) error = NULL;

  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  const gchar *app_id = xdp_app_info_get_id (app_info);

  options = create_credential_validate_options (arg_options, &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_dex_new (
                                  credential->context,
                                  app_info,
                                  G_DBUS_INTERFACE_SKELETON (object),
                                  G_DBUS_PROXY (credential->handler),
                                  options
                              ),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_experimental_credential_complete_create_credential (
      object,
      invocation,
      xdp_request_dex_get_object_path (request));

  {
    g_autoptr (XdpDbusExperimentalHandlerCredentialCreateCredentialResult) result = NULL;
    result = dex_await_boxed (xdp_dbus_experimental_handler_credential_call_create_credential_future (
        credential->handler,
        arg_parent_window,
        arg_origin,
        arg_type,
        options,
        app_id
      ),
      &error);

    if (result) {
      xdp_request_dex_emit_response (request,
                                     result->response,
                                     result->results);
    } else {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Handler call failed: %s (%d)", error->message, error->code);
      xdp_request_dex_emit_response (request,
                                     XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                     NULL);
    }
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey get_credential_options[] = {
    {"handle_token", G_VARIANT_TYPE_STRING, NULL},
    {"origin", G_VARIANT_TYPE_STRING, NULL},
    {"top_origin", G_VARIANT_TYPE_STRING, NULL},
    {"public_key", G_VARIANT_TYPE_STRING, NULL},
};

/**
 * get_credential_validate_options:
 * @arg_options: (transfer none): options passed to the frontend.
 * @error: (transfer none): pointer to an error pointer that will be populated on error.
 * Returns: (transfer full): Filtered list of options to pass to the handler.
 */
static GVariant *
get_credential_validate_options (GVariant *arg_options,
                                 GError  **error)
{
  g_auto (GVariantBuilder) options = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  gboolean validated = xdp_filter_options (arg_options,
                                           &options,
                                           get_credential_options,
                                           G_N_ELEMENTS (get_credential_options),
                                           NULL,
                                           error);
  if (!validated)
      return NULL;
  else
      return g_variant_ref_sink (g_variant_builder_end (&options));
}

static gboolean handle_get_credential (XdpDbusExperimentalCredential *object,
                                       GDBusMethodInvocation         *invocation,
                                       const gchar                   *arg_parent_window,
                                       const gchar                   *arg_origin,
                                       GVariant                      *arg_options)
{
  XdpCredential *credential = XDP_CREDENTIAL (object);
  g_autoptr (XdpRequestDex) request = NULL;
  g_autoptr (GVariant) options = NULL;
  g_autoptr (GError) error = NULL;

  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  const gchar *app_id = xdp_app_info_get_id (app_info);

  options = get_credential_validate_options (arg_options, &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_dex_new (
                                  credential->context,
                                  app_info,
                                  G_DBUS_INTERFACE_SKELETON (object),
                                  G_DBUS_PROXY (credential->handler),
                                  options
                              ),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_experimental_credential_complete_get_credential (
      object,
      invocation,
      xdp_request_dex_get_object_path (request));

  {
    g_autoptr (XdpDbusExperimentalHandlerCredentialGetCredentialResult) result = NULL;
    result = dex_await_boxed (xdp_dbus_experimental_handler_credential_call_get_credential_future (
        credential->handler,
        arg_parent_window,
        arg_origin,
        options,
        app_id
      ),
      &error);

    if (result) {
      xdp_request_dex_emit_response (request,
                                     result->response,
                                     result->results);
    } else {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Handler call failed: %s (%d)", error->message, error->code);
      xdp_request_dex_emit_response (request,
                                    XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                    NULL);
    }
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

DexFuture *
init_credential (gpointer user_data)
{
  g_info ("Initializing Credential Portal");

  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr (XdpCredential) credential = NULL;
  g_autoptr (XdpDbusExperimentalHandlerCredential) handler = NULL;
  g_autoptr (GError) error = NULL;

  GDBusConnection *connection = xdp_context_get_connection (context);
  {
    XdpPortalConfig *config = xdp_context_get_config (context);
    XdpImplConfig *impl_config;

    impl_config =
        xdp_portal_config_find (config, CREDENTIAL_EXPERIMENTAL_DBUS_IMPL_IFACE);

    if (impl_config == NULL) {
      g_debug ("impl_config is NULL");
      return dex_future_new_true ();
    }

    g_debug ("found impl");
  }

  g_debug ("creating handler proxy...");

  handler = dex_await_object (xdp_dbus_experimental_handler_credential_proxy_new_future (
                                  connection,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  CREDENTIALSD_HANDLER_DBUS_NAME,
                                  DESKTOP_DBUS_PATH),
                              &error);

  if (!handler)
    {
      g_warning ("Failed to create credential proxy: %s", error->message);
      return dex_future_new_false ();
    }
  g_debug ("created handler proxy.");

  credential = xdp_credential_new (context, g_steal_pointer (&handler));

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&credential)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  return dex_future_new_true ();
}
