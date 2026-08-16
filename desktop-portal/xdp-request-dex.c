/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-request-dex.h"

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

enum
{
  REQUEST_CLOSED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct _XdpRequestDex
{
  XdpDbusRequestSkeleton parent_instance;

  XdpContext *context;
  XdpAppInfo *app_info;
  XdpDbusImplRequest *impl_request;
  GDBusConnection *connection;
  char *id;
  gboolean exported;
  gboolean claimed;
  gboolean closed_emitted;
} XdpRequestDex;

static void xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpRequestDex,
                               xdp_request_dex,
                               XDP_DBUS_TYPE_REQUEST_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REQUEST,
                                                      xdp_request_skeleton_iface_init))

static void
xdp_request_dex_release_path (XdpRequestDex *request)
{
  if (!request->claimed)
    return;

  xdp_context_unclaim_object_path (request->context, request->id);
  request->claimed = FALSE;
}

static void
xdp_request_dex_unexport (XdpRequestDex *request,
                          gboolean       release_path)
{
  if (request->exported)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
      request->exported = FALSE;
    }

  if (!request->closed_emitted)
    {
      request->closed_emitted = TRUE;
      g_signal_emit (request, signals[REQUEST_CLOSED], 0);
    }

  if (release_path)
    xdp_request_dex_release_path (request);
}

void
xdp_request_dex_close (XdpRequestDex *request)
{
  g_return_if_fail (XDP_IS_REQUEST_DEX (request));

  if (!request->exported)
    return;

  xdp_request_dex_unexport (request, TRUE);
  xdp_dbus_impl_request_call_close (request->impl_request, NULL, NULL, NULL);
}

static void
xdp_request_dex_on_signal_response (XdpDbusRequest *object,
                                    guint           arg_response,
                                    GVariant       *arg_results)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (object);

  if (!request->connection)
    return;

  g_dbus_connection_emit_signal (request->connection,
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

  xdp_request_dex_unexport (request, TRUE);

  dex_await (xdp_dbus_impl_request_call_close_future (request->impl_request),
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

  xdp_request_dex_close (request);

  xdp_request_dex_release_path (request);

  g_clear_object (&request->app_info);
  g_clear_object (&request->impl_request);
  g_clear_object (&request->connection);
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

  signals[REQUEST_CLOSED] =
    g_signal_new ("request-closed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
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

typedef struct _RequestImplProxyCreateData {
  XdpContext *context;
  XdpAppInfo *app_info;
  GDBusConnection *connection;
  char *id;
  char *peer;
  gulong peer_disconnect_handler;
  gboolean peer_disconnected;
} RequestImplProxyCreateData;

static void
on_pending_peer_disconnect (XdpContext *context,
                            const char *peer,
                            gpointer    user_data)
{
  RequestImplProxyCreateData *data = user_data;

  if (g_strcmp0 (data->peer, peer) == 0)
    data->peer_disconnected = TRUE;
}

static void
request_impl_proxy_create_data_free (RequestImplProxyCreateData *data)
{
  if (data->context != NULL)
    g_clear_signal_handler (&data->peer_disconnect_handler, data->context);
  if (data->context && data->id)
    xdp_context_unclaim_object_path (data->context, data->id);

  g_clear_object (&data->app_info);
  g_clear_object (&data->connection);
  g_clear_pointer (&data->id, g_free);
  g_clear_pointer (&data->peer, g_free);
  free (data);
}

static void
on_peer_disconnect (XdpContext *context,
                    const char *peer,
                    gpointer    user_data)
{
  g_autoptr(XdpRequestDex) request = g_object_ref (XDP_REQUEST_DEX (user_data));

  if (g_strcmp0 (xdp_app_info_get_sender (request->app_info), peer) != 0)
    return;

  if (!request->exported)
    return;

  xdp_request_dex_close (request);
}

static DexFuture *
on_impl_request_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  RequestImplProxyCreateData *data = user_data;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;

  if (data->peer_disconnected)
    return dex_future_new_for_error (
      g_error_new_literal (G_IO_ERROR,
                           G_IO_ERROR_CANCELLED,
                           "Caller disconnected"));

  impl_request = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_request);

  request = g_object_new (XDP_TYPE_REQUEST_DEX, NULL);
  request->context = g_steal_pointer (&data->context);
  request->app_info = g_steal_pointer (&data->app_info);
  request->impl_request = g_steal_pointer (&impl_request);
  request->connection = g_steal_pointer (&data->connection);
  request->id = g_steal_pointer (&data->id);
  request->claimed = TRUE;

  g_signal_connect_object (request->context, "peer-disconnect",
                           G_CALLBACK (on_peer_disconnect),
                           request,
                           G_CONNECT_DEFAULT);

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (request),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                          request->connection,
                                          request->id,
                                          &error))
    {
      g_clear_signal_handler (&data->peer_disconnect_handler,
                              request->context);
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  request->exported = TRUE;
  g_clear_signal_handler (&data->peer_disconnect_handler, request->context);
  if (data->peer_disconnected)
    {
      xdp_request_dex_unexport (request, TRUE);
      xdp_dbus_impl_request_call_close (request->impl_request, NULL, NULL, NULL);
      return dex_future_new_for_error (
        g_error_new_literal (G_IO_ERROR,
                             G_IO_ERROR_CANCELLED,
                             "Caller disconnected"));
    }

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
  RequestImplProxyCreateData *data;
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

  future = xdp_dbus_impl_request_proxy_new_future (
    g_dbus_proxy_get_connection (proxy_impl),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (proxy_impl),
    id);

  data = g_new0 (RequestImplProxyCreateData, 1);
  data->context = context;
  data->app_info = g_object_ref (app_info);
  data->connection = g_object_ref (
    g_dbus_interface_skeleton_get_connection (skeleton));
  data->id = g_steal_pointer (&id);
  data->peer = g_strdup (xdp_app_info_get_sender (app_info));
  data->peer_disconnect_handler =
    g_signal_connect (context,
                      "peer-disconnect",
                      G_CALLBACK (on_pending_peer_disconnect),
                      data);

  future = dex_future_then (future,
                            on_impl_request_proxy_created,
                            g_steal_pointer (&data),
                            (GDestroyNotify) request_impl_proxy_create_data_free);

  return g_steal_pointer (&future);
}

void
xdp_request_dex_emit_response (XdpRequestDex                *request,
                               XdgDesktopPortalResponseEnum  response,
                               GVariant                     *results)
{
  if (!request->exported)
    return;

  if (!request->connection)
    return;

  if (!results)
    {
      g_auto(GVariantBuilder) empty_results_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      results = g_variant_builder_end (&empty_results_builder);
    }

  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  response,
                                  results);
  xdp_request_dex_unexport (request, TRUE);
}

const char *
xdp_request_dex_get_object_path (XdpRequestDex *request)
{
  return request->id;
}
