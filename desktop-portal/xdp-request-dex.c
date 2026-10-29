/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-request-dex.h"

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-impl-owner-watch.h"
#include "xdp-utils.h"

typedef struct _XdpRequestDex
{
  XdpDbusRequestSkeleton parent_instance;

  XdpContext *context;
  XdpAppInfo *app_info;
  XdpDbusImplRequest *impl_request;
  XdpImplOwnerWatch *impl_owner_watch;
  GDBusInterfaceSkeleton *skeleton;
  char *id;
  gboolean exported;
  gboolean path_claimed;
  gboolean responded;
} XdpRequestDex;

static void xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpRequestDex,
                               xdp_request_dex,
                               XDP_DBUS_TYPE_REQUEST_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REQUEST,
                                                      xdp_request_skeleton_iface_init))

static void
xdp_request_dex_unexport (XdpRequestDex *request)
{
  if (request->exported)
    {
      request->exported = FALSE;
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
    }

  if (request->path_claimed)
    {
      request->path_claimed = FALSE;
      xdp_context_unclaim_object_path (request->context, request->id);
    }
}

static void
xdp_request_dex_on_signal_response (XdpDbusRequest *object,
                                    guint           arg_response,
                                    GVariant       *arg_results)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (object);
  GDBusConnection *connection;

  connection = g_dbus_interface_skeleton_get_connection (request->skeleton);
  if (!connection)
    return;

  g_dbus_connection_emit_signal (connection,
                                 xdp_app_info_get_sender (request->app_info),
                                 request->id,
                                 DESKTOP_DBUS_IFACE ".Request",
                                 "Response",
                                 g_variant_new ("(u@a{sv})",
                                                arg_response,
                                                arg_results),
                                 NULL);
}

static gboolean
xdp_request_dex_handle_close (XdpDbusRequest        *object,
                              GDBusMethodInvocation *invocation)
{
  g_autoptr(XdpRequestDex) request = g_object_ref (XDP_REQUEST_DEX (object));
  g_autoptr(GError) error = NULL;

  if (!request->exported)
    {
      xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_dex_unexport (request);

  dex_await (xdp_impl_owner_watch_close_future (request->impl_owner_watch,
                                               G_DBUS_PROXY (request->impl_request)),
             &error);
  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface)
{
  iface->handle_close = xdp_request_dex_handle_close;
  iface->response = xdp_request_dex_on_signal_response;
}

static void
xdp_request_dex_dispose (GObject *object)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (object);

  if (request->exported)
    {
      if (!request->responded)
        xdp_request_dex_emit_response (request,
                                       XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                       NULL);

      xdp_impl_owner_watch_close (request->impl_owner_watch,
                                  G_DBUS_PROXY (request->impl_request));
    }

  xdp_request_dex_unexport (request);
  g_clear_object (&request->impl_owner_watch);
  g_clear_object (&request->app_info);
  g_clear_object (&request->impl_request);
  g_clear_object (&request->skeleton);
  g_clear_pointer (&request->id, g_free);

  G_OBJECT_CLASS (xdp_request_dex_parent_class)->dispose (object);
}

static void
xdp_request_dex_init (XdpRequestDex *request)
{
}

static void
xdp_request_dex_class_init (XdpRequestDexClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_request_dex_dispose;
}

static gboolean
request_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (user_data);
  const char *request_sender = xdp_app_info_get_sender (request->app_info);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_strcmp0 (sender, request_sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

static void
on_peer_disconnect (XdpContext *context,
                    const char *peer,
                    gpointer    user_data)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (user_data);

  if (g_strcmp0 (xdp_app_info_get_sender (request->app_info), peer) != 0)
    return;

  if (!request->exported)
    return;

  xdp_impl_owner_watch_close (request->impl_owner_watch,
                              G_DBUS_PROXY (request->impl_request));

  xdp_request_dex_unexport (request);
}

static void
on_impl_owner_lost (XdpImplOwnerWatch *watch,
                    GObject           *object)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (object);

  if (!request->exported)
    return;

  if (!request->responded)
    xdp_request_dex_emit_response (request,
                                   XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                   NULL);

  xdp_request_dex_unexport (request);
}

static DexFuture *
on_impl_request_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (user_data);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;

  impl_request = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_request);

  request->impl_request = g_steal_pointer (&impl_request);

  return dex_future_new_for_object (request);
}

DexFuture *
xdp_request_dex_new (XdpContext             *context,
                     XdpAppInfo             *app_info,
                     GDBusInterfaceSkeleton *skeleton,
                     GDBusProxy             *proxy_impl,
                     GVariant               *arg_options)
{
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(XdpRequestDex) request = NULL;
  const char *token = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *id = NULL;

  g_variant_lookup (arg_options, "handle_token", "&s", &token);
  token = token ? token : "t";
  if (!xdp_is_valid_token (token))
    {
      return dex_future_new_for_error (g_error_new (XDG_DESKTOP_PORTAL_ERROR,
                                                    XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                    "Invalid token: %s", token));
    }

  sender = g_strdup (xdp_app_info_get_sender (app_info) + 1);
  for (size_t i = 0; sender[i]; i++)
    {
      if (sender[i] == '.')
        sender[i] = '_';
    }

  id = g_strdup_printf (DESKTOP_DBUS_PATH "/request/%s/%s", sender, token);

  while (!xdp_context_claim_object_path (context, id))
    {
      uint32_t r = g_random_int ();
      g_free (id);
      id = g_strdup_printf (DESKTOP_DBUS_PATH "/request/%s/%s/%u",
                            sender,
                            token,
                            r);
    }

  request = g_object_new (XDP_TYPE_REQUEST_DEX, NULL);
  request->context = context;
  request->app_info = g_object_ref (app_info);
  request->skeleton = g_object_ref (skeleton);
  request->id = g_steal_pointer (&id);
  request->path_claimed = TRUE;
  request->impl_owner_watch =
    xdp_impl_owner_watch_new (proxy_impl, G_OBJECT (request), on_impl_owner_lost);

  g_signal_connect_object (request->context, "peer-disconnect",
                           G_CALLBACK (on_peer_disconnect),
                           request,
                           G_CONNECT_DEFAULT);

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (request),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request);

  future = xdp_dbus_impl_request_proxy_new_future (
    g_dbus_proxy_get_connection (proxy_impl),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (proxy_impl),
    request->id);

  future = dex_future_then (future,
                            on_impl_request_proxy_created,
                            g_steal_pointer (&request),
                            g_object_unref);

  return g_steal_pointer (&future);
}

gboolean
xdp_request_dex_export (XdpRequestDex  *request,
                        GError        **error)
{
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         g_dbus_interface_skeleton_get_connection (request->skeleton),
                                         request->id,
                                         error))
    return FALSE;

  request->exported = TRUE;

  if (!xdp_impl_owner_watch_is_current (request->impl_owner_watch))
    on_impl_owner_lost (request->impl_owner_watch, G_OBJECT (request));

  return TRUE;
}

void
xdp_request_dex_emit_response (XdpRequestDex                *request,
                               XdgDesktopPortalResponseEnum  response,
                               GVariant                     *results)
{
  if (!request->exported)
    return;

  g_return_if_fail (!request->responded);

  if (!xdp_impl_owner_watch_is_current (request->impl_owner_watch))
    response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;

  if (!g_dbus_interface_skeleton_get_connection (request->skeleton))
    return;

  if (!results)
    {
      g_auto(GVariantBuilder) empty_results_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      results = g_variant_builder_end (&empty_results_builder);
    }

  request->responded = TRUE;

  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  response,
                                  results);
}

const char *
xdp_request_dex_get_object_path (XdpRequestDex *request)
{
  return request->id;
}
