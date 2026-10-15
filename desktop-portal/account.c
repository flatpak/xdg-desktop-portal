/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "account.h"

#include <gio/gio.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-documents.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request-dex.h"
#include "xdp-utils.h"

struct _XdpAccount
{
  XdpDbusAccountSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplAccount *impl;
};

#define XDP_TYPE_ACCOUNT (xdp_account_get_type ())
G_DECLARE_FINAL_TYPE (XdpAccount,
                      xdp_account,
                      XDP, ACCOUNT,
                      XdpDbusAccountSkeleton)

G_DEFINE_FINAL_TYPE (XdpAccount,
                     xdp_account,
                     XDP_DBUS_TYPE_ACCOUNT_SKELETON);

static gboolean
validate_reason (const char  *key,
                 GVariant    *value,
                 GVariant    *options,
                 gpointer     user_data,
                 GError     **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (g_utf8_strlen (string, -1) > 256)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "Not accepting overly long reasons");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey user_information_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason },
};

static gboolean
handle_get_user_information (XdpDbusAccount        *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_parent_window,
                             GVariant              *arg_options)
{
  XdpAccount *account = XDP_ACCOUNT (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplAccountGetUserInformationResult) result = NULL;
  g_auto(GVariantBuilder) new_results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) idv = NULL;
  g_autoptr(GVariant) namev = NULL;
  unsigned int impl_request_version;
  const char *image;

  {
    g_auto(GVariantBuilder) options_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    xdp_filter_options (arg_options, &options_builder,
                        user_information_options, G_N_ELEMENTS (user_information_options),
                        NULL, NULL);
    options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  }

  impl_request_version = xdp_dbus_impl_account_get_request_version (account->impl);

  request = dex_await_object (xdp_request_dex_new (account->context,
                                                   app_info,
                                                   G_DBUS_INTERFACE_SKELETON (object),
                                                   G_DBUS_PROXY (account->impl),
                                                   impl_request_version,
                                                   arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_autoptr(DexFuture) impl_future = NULL;

    impl_future = xdp_dbus_impl_account_call_get_user_information_future (
        account->impl,
        xdp_request_dex_get_object_path (request),
        xdp_app_info_get_id (app_info),
        arg_parent_window,
        options);

    dex_await (xdp_request_dex_export (request), NULL);

    xdp_dbus_account_complete_get_user_information (object,
                                                    g_steal_pointer (&invocation),
                                                    xdp_request_dex_get_object_path (request));

    result = dex_await_boxed (g_steal_pointer (&impl_future), &error);
  }

  if (!result)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    {
      xdp_request_dex_emit_response (request, result->response, NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  idv = g_variant_lookup_value (result->results, "id", G_VARIANT_TYPE_STRING);
  namev = g_variant_lookup_value (result->results, "name", G_VARIANT_TYPE_STRING);

  g_variant_builder_add (&new_results, "{sv}", "id", idv);
  g_variant_builder_add (&new_results, "{sv}", "name", namev);

  if (g_variant_lookup (result->results, "image", "&s", &image))
    {
      g_autofree char *ruri = NULL;
      g_autoptr(GError) image_error = NULL;

      if (xdp_app_info_is_host (app_info))
        ruri = g_strdup (image);
      else
        ruri = xdp_register_document (image,
                                      xdp_app_info_get_id (app_info),
                                      xdp_app_info_get_gappinfo (app_info),
                                      XDP_DOCUMENT_FLAG_NONE,
                                      &image_error);

      if (ruri == NULL)
        g_warning ("Failed to register %s: %s", image, image_error->message);
      else
        g_variant_builder_add (&new_results, "{sv}", "image", g_variant_new_string (ruri));
    }

  xdp_request_dex_emit_response (request, result->response,
                                 g_variant_builder_end (&new_results));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_account_dispose (GObject *object)
{
  XdpAccount *account = XDP_ACCOUNT (object);

  g_clear_object (&account->impl);

  G_OBJECT_CLASS (xdp_account_parent_class)->dispose (object);
}

static void
xdp_account_init (XdpAccount *account)
{
}

static void
xdp_account_class_init (XdpAccountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_account_dispose;
}

static XdpAccount *
xdp_account_new (XdpContext         *context,
                 XdpDbusImplAccount *impl)
{
  XdpAccount *account;

  account = g_object_new (XDP_TYPE_ACCOUNT, NULL);
  account->context = context;
  account->impl = g_object_ref (impl);

  g_signal_connect (account, "handle-get-user-information",
                    G_CALLBACK (handle_get_user_information), NULL);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (account->impl), G_MAXINT);

  xdp_dbus_account_set_version (XDP_DBUS_ACCOUNT (account), 1);

  return account;
}

DexFuture *
init_account (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpAccount) account = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplAccount) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, ACCOUNT_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_account_proxy_new_future (connection,
                                                                   G_DBUS_PROXY_FLAGS_NONE,
                                                                   impl_config->dbus_name,
                                                                   DESKTOP_DBUS_PATH),
                           &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create account proxy: %s", error->message);
      return dex_future_new_false ();
    }

  account = xdp_account_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&account)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  return dex_future_new_true ();
}
