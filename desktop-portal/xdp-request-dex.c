/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-request-dex.h"

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _XdpRequestDex
{
  XdpDbusRequestSkeleton parent_instance;

  XdpContext *context;
  XdpAppInfo *app_info;
  XdpDbusImplRequest *impl_request;
  GDBusInterfaceSkeleton *skeleton;
  char *id;
  unsigned int impl_request_version;
  gboolean exported;
  gboolean responded;
} XdpRequestDex;

/**
 * XdpRequestDex:
 *
 * A future-based Request implementation.
 *
 * ## Lifecycle
 *
 * [ctor@RequestDex.new] creates the request and a proxy for the impl
 * request, but does not export the request on the bus.
 *
 * The request must only be exported after the backend has created the
 * impl request object, because a `Close` call on the exported request
 * is forwarded to the impl request. [method@RequestDex.export]
 * handles this: when `impl_request_version` (passed to
 * [ctor@RequestDex.new]) is 2 or higher, the returned future resolves
 * only after the backend emits the `Created` signal on the impl
 * request. With older backends the future resolves immediately; there
 * is a small race in that case where a `Close` from the app may
 * arrive before the backend has created the impl request. The caller
 * should pass the `request-version` property from the impl portal
 * interface.
 *
 * The D-Bus method call must only be completed to the app after the
 * export, so that the request object path the app receives is
 * immediately usable for `Close`.
 *
 * ## Response handling
 *
 * Use [method@RequestDex.emit_response] to send a `Response` signal
 * to the app. Calling it more than once is a programming error.
 *
 * If the request is disposed without a response having been sent
 * (e.g. on error paths), dispose automatically emits
 * `XDG_DESKTOP_PORTAL_RESPONSE_OTHER`. Portal handlers can therefore
 * simply return on failure and let the `g_autoptr` cleanup take care
 * of notifying the app.
 *
 * ## Long-lived call pattern
 *
 * When the backend impl call blocks for the entire duration of the
 * request (e.g. waiting for user interaction), the portal must issue
 * the backend call first, then await the export, then complete the
 * D-Bus method call, and finally await the backend result:
 *
 * ```c
 * request = dex_await_object (xdp_request_dex_new (...), &error);
 * impl_future = xdp_dbus_impl_foo_call_bar_future (impl, ...,
 *     xdp_request_dex_get_object_path (request), ...);
 * dex_await (xdp_request_dex_export (request), NULL);
 * xdp_dbus_foo_complete_bar (object, g_steal_pointer (&invocation),
 *     xdp_request_dex_get_object_path (request));
 * result = dex_await_boxed (g_steal_pointer (&impl_future), &error);
 * ```
 *
 * The backend call must be issued before the export so that the
 * export's `Created` signal wait does not deadlock: the backend
 * creates the impl request (and emits `Created`) only in response
 * to the method call.
 *
 * ## Immediate-return pattern
 *
 * When the backend impl call returns immediately and the request
 * lives until something else happens, the reply itself proves the
 * impl request exists. The portal can await the reply, then export:
 *
 * ```c
 * request = dex_await_object (xdp_request_dex_new (...), &error);
 * dex_await (xdp_dbus_impl_foo_call_bar_future (impl, ...,
 *     xdp_request_dex_get_object_path (request), ...),
 *     &error);
 * dex_await (xdp_request_dex_export (request), NULL);
 * xdp_dbus_foo_complete_bar (object, g_steal_pointer (&invocation),
 *     xdp_request_dex_get_object_path (request));
 * ```
 *
 * This pattern has no race because the method reply guarantees the
 * backend has created the impl request object.
 */

static void xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpRequestDex,
                               xdp_request_dex,
                               XDP_DBUS_TYPE_REQUEST_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REQUEST,
                                                      xdp_request_skeleton_iface_init))

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
  XdpRequestDex *request = XDP_REQUEST_DEX (object);
  g_autoptr(GError) error = NULL;

  if (!request->exported)
    {
      xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  request->exported = FALSE;
  xdp_context_unclaim_object_path (request->context, request->id);

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

  if (request->exported)
    {
      if (!request->responded)
        xdp_request_dex_emit_response (request,
                                       XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                       NULL);

      xdp_dbus_impl_request_call_close (request->impl_request, NULL, NULL, NULL),

      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
      request->exported = FALSE;
      xdp_context_unclaim_object_path (request->context, request->id);
    }

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

typedef struct _RequestImplProxyCreateData {
  XdpContext *context;
  XdpAppInfo *app_info;
  GDBusInterfaceSkeleton *skeleton;
  char *id;
  unsigned int impl_request_version;
} RequestImplProxyCreateData;

static void
request_impl_proxy_create_data_free (RequestImplProxyCreateData *data)
{
  g_clear_object (&data->app_info);
  g_clear_object (&data->skeleton);
  g_clear_pointer (&data->id, g_free);
  free (data);
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

  xdp_dbus_impl_request_call_close (request->impl_request, NULL, NULL, NULL),

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  request->exported = FALSE;
  xdp_context_unclaim_object_path (request->context, request->id);
}

static DexFuture *
on_impl_request_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  RequestImplProxyCreateData *data = user_data;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;

  impl_request = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_request);

  request = g_object_new (XDP_TYPE_REQUEST_DEX, NULL);
  request->context = g_steal_pointer (&data->context);
  request->app_info = g_steal_pointer (&data->app_info);
  request->impl_request = g_steal_pointer (&impl_request);
  request->skeleton = g_steal_pointer (&data->skeleton);
  request->id = g_steal_pointer (&data->id);
  request->impl_request_version = data->impl_request_version;
  g_signal_connect_object (request->context, "peer-disconnect",
                           G_CALLBACK (on_peer_disconnect),
                           request,
                           G_CONNECT_DEFAULT);

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (request),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request);

  return dex_future_new_for_object (request);
}

DexFuture *
xdp_request_dex_new (XdpContext             *context,
                     XdpAppInfo             *app_info,
                     GDBusInterfaceSkeleton *skeleton,
                     GDBusProxy             *proxy_impl,
                     unsigned int            impl_request_version,
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
  data->skeleton = g_object_ref (skeleton);
  data->id = g_steal_pointer (&id);
  data->impl_request_version = impl_request_version;

  future = dex_future_then (future,
                            on_impl_request_proxy_created,
                            g_steal_pointer (&data),
                            (GDestroyNotify) request_impl_proxy_create_data_free);

  return g_steal_pointer (&future);
}

static DexFuture *
do_export (DexFuture *future,
           gpointer   user_data)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (user_data);
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         g_dbus_interface_skeleton_get_connection (request->skeleton),
                                         request->id,
                                         &error))
    return dex_future_new_for_error (g_steal_pointer (&error));

  request->exported = TRUE;
  return dex_future_new_for_boolean (TRUE);
}

DexFuture *
xdp_request_dex_export (XdpRequestDex *request)
{
  DexFuture *precondition;

  if (request->impl_request_version >= 2)
    precondition = xdp_dbus_impl_request_wait_created_future (request->impl_request);
  else
    precondition = dex_future_new_for_boolean (TRUE);

  return dex_future_then (precondition,
                          do_export,
                          g_object_ref (request),
                          g_object_unref);
}

void
xdp_request_dex_emit_response (XdpRequestDex                *request,
                               XdgDesktopPortalResponseEnum  response,
                               GVariant                     *results)
{
  g_return_if_fail (!request->responded);

  if (!request->exported)
    return;

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
