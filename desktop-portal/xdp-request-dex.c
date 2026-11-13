/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-request-dex.h"

#include "xdp-app-info-registry.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _XdpRequestDex
{
  XdpDbusRequestSkeleton parent_instance;

  XdpContext *context; /* (unowned) (not nullable) */
  XdpAppInfo *app_info; /* (owned) (not nullable) */
  XdpDbusImplRequest *impl_request; /* (owned) (nullable) */
  GDBusInterfaceSkeleton *skeleton; /* (owned) (not nullable) */
  XdpObjectPathClaim *claim; /* (owned) (not nullable) */
  DexPromise *close_promise; /* (owned) (not nullable) */
  DexPromise *cancel_promise; /* (owned) (not nullable) */
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
 * request. It also starts discovering the impl request object on the
 * bus (via the `Created` signal or polling) and sets up a close chain
 * that forwards `Close` to the impl once both the impl is discovered
 * and a close has been requested.
 *
 * [method@RequestDex.export] exports the request on the bus
 * immediately. It does not wait for the impl request to appear.
 *
 * [method@RequestDex.close] unexports the request and resolves the
 * close promise. The impl `Close` call runs asynchronously in the
 * background once the impl request has been discovered.
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
 * ## Usage pattern
 *
 * ```c
 * request = dex_await_object (xdp_request_dex_new (...), &error);
 * xdp_request_dex_export (request, NULL);
 * xdp_dbus_foo_complete_bar (object,
 *                            g_steal_pointer (&invocation),
 *                            xdp_request_dex_get_object_path (request));
 * result = dex_await_boxed (xdp_dbus_impl_foo_call_bar_future (
 *     impl,
 *     ...,
 *     xdp_request_dex_get_object_path (request),
 *     ...),
 *   &error);
 * ```
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
                                 xdp_request_dex_get_object_path (request),
                                 DESKTOP_DBUS_IFACE ".Request",
                                 "Response",
                                 g_variant_new ("(u@a{sv})",
                                                arg_response,
                                                arg_results),
                                 NULL);
}

void
xdp_request_dex_close (XdpRequestDex *request)
{
  if (request->exported)
    {
      request->exported = FALSE;
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
    }

  if (!dex_future_is_resolved (DEX_FUTURE (request->close_promise)))
    dex_promise_resolve_boolean (request->close_promise, TRUE);
}

static gboolean
xdp_request_dex_handle_close (XdpDbusRequest        *object,
                              GDBusMethodInvocation *invocation)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (object);

  xdp_request_dex_close (request);
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

  if (request->exported && !request->responded)
    xdp_request_dex_emit_response (request,
                                   XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                   NULL);

  xdp_request_dex_close (request);

  if (!dex_future_is_resolved (DEX_FUTURE (request->cancel_promise)))
    dex_promise_resolve_boolean (request->cancel_promise, TRUE);

  dex_clear (&request->close_promise);
  dex_clear (&request->cancel_promise);
  g_clear_pointer (&request->claim, xdp_object_path_claim_unref);
  g_clear_object (&request->app_info);
  g_clear_object (&request->impl_request);
  g_clear_object (&request->skeleton);

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

typedef struct _PollData {
  GDBusProxy *proxy; /* (owned) (not nullable) */
  unsigned int poll_delay_ms;
  unsigned int poll_total_ms;
} PollData;

static void
poll_data_free (PollData *data)
{
  g_clear_object (&data->proxy);
  g_free (data);
}

#define IMPL_REQUEST_POLL_INITIAL_DELAY_MS 100
#define IMPL_REQUEST_POLL_MAX_TOTAL_MS 30000

static DexFuture *
check_impl_request_available (DexFuture *completed,
                              gpointer   user_data)
{
  PollData *pd = user_data;
  GDBusConnection *connection = g_dbus_proxy_get_connection (pd->proxy);
  const char *name = g_dbus_proxy_get_name (pd->proxy);
  const char *path = g_dbus_proxy_get_object_path (pd->proxy);

  return dex_dbus_connection_call (connection, name, path,
                                   "org.freedesktop.DBus.Properties",
                                   "GetAll",
                                   g_variant_new ("(s)",
                                                  "org.freedesktop.impl.portal.Request"),
                                   G_VARIANT_TYPE ("(a{sv})"),
                                   G_DBUS_CALL_FLAGS_NONE, -1);
}

static DexFuture *
retry_check_impl_request_poll (DexFuture *completed,
                               gpointer   user_data)
{
  PollData *pd = user_data;
  g_autoptr(GError) error = NULL;
  unsigned int delay_ms;

  dex_future_get_value (completed, &error);

  if (!g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) &&
      !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT))
    return dex_future_new_for_error (g_steal_pointer (&error));

  if (pd->poll_total_ms >= IMPL_REQUEST_POLL_MAX_TOTAL_MS)
    {
      g_warning ("Impl request at %s did not appear after %ums",
                 g_dbus_proxy_get_object_path (pd->proxy),
                 pd->poll_total_ms);
      return dex_future_new_for_boolean (TRUE);
    }

  delay_ms = pd->poll_delay_ms;
  pd->poll_total_ms += delay_ms;
  pd->poll_delay_ms = delay_ms * 2;

  g_debug ("Impl request at %s does not exist yet, retrying in %ums",
           g_dbus_proxy_get_object_path (pd->proxy),
           delay_ms);

  return dex_future_catch (dex_timeout_new_usec (delay_ms * 1000),
                           check_impl_request_available,
                           user_data, NULL);
}

static DexFuture *
do_impl_close (DexFuture *completed,
               gpointer   user_data)
{
  XdpDbusImplRequest *impl_request = XDP_DBUS_IMPL_REQUEST (user_data);

  return xdp_dbus_impl_request_call_close_future (impl_request);
}

static void
setup_impl_close_chain (XdpRequestDex *request)
{
  PollData *pd;
  DexFuture *signal;
  DexFuture *poll;
  DexFuture *impl_discovery;
  DexFuture *close_trigger;
  DexFuture *close_chain;

  pd = g_new0 (PollData, 1);
  pd->proxy = g_object_ref (G_DBUS_PROXY (request->impl_request));
  pd->poll_delay_ms = IMPL_REQUEST_POLL_INITIAL_DELAY_MS;
  pd->poll_total_ms = 0;

  /* The Created signal (v2) reliably detects the impl request. */
  signal = xdp_dbus_impl_request_wait_created_future (request->impl_request);

  /* The poll fallback (v1) can miss the impl request if the backend
   * exports and unexports it between polls. That is acceptable
   * because we can only miss it if it already went away on its own,
   * so there is nothing left to close. */
  poll = dex_future_catch_loop (check_impl_request_available (NULL, pd),
                                retry_check_impl_request_poll,
                                pd,
                                (GDestroyNotify) poll_data_free);

  impl_discovery = dex_future_any (signal, poll,
                                   DEX_FUTURE (dex_ref (request->cancel_promise)),
                                   NULL);

  /* Forward Close to the impl once both the client has requested
   * close AND the impl request has been discovered on the bus. */
  close_trigger = dex_future_all (
      DEX_FUTURE (dex_ref (request->close_promise)),
      impl_discovery,
      NULL);

  close_chain = dex_future_then (close_trigger,
                                 do_impl_close,
                                 g_object_ref (request->impl_request),
                                 g_object_unref);

  dex_future_disown (close_chain);
}

typedef struct _RequestImplProxyCreateData {
  XdpContext *context;
  XdpAppInfo *app_info;
  GDBusInterfaceSkeleton *skeleton;
  XdpObjectPathClaim *claim;
} RequestImplProxyCreateData;

static void
request_impl_proxy_create_data_free (RequestImplProxyCreateData *data)
{
  g_clear_pointer (&data->claim, xdp_object_path_claim_unref);
  g_clear_object (&data->app_info);
  g_clear_object (&data->skeleton);
  g_free (data);
}

static void
on_app_info_removed (XdpAppInfoRegistry *registry,
                     XdpAppInfo         *app_info,
                     gpointer            user_data)
{
  XdpRequestDex *request = XDP_REQUEST_DEX (user_data);

  if (app_info != request->app_info)
    return;

  xdp_request_dex_close (request);
}

static DexFuture *
xdp_request_dex_new_inner (XdpContext             *context,
                           XdpAppInfo             *app_info,
                           GDBusInterfaceSkeleton *skeleton,
                           XdpDbusImplRequest     *impl_request,
                           XdpObjectPathClaim     *claim)
{
  XdpAppInfoRegistry *registry =
    xdp_context_get_app_info_registry (context);
  g_autoptr(XdpRequestDex) request = NULL;

  request = g_object_new (XDP_TYPE_REQUEST_DEX, NULL);
  request->context = context;
  request->app_info = g_object_ref (app_info);
  request->skeleton = g_object_ref (skeleton);
  request->claim = xdp_object_path_claim_ref (claim);
  request->close_promise = dex_promise_new ();
  request->cancel_promise = dex_promise_new ();

  if (impl_request)
    request->impl_request = g_object_ref (impl_request);

  g_signal_connect_object (registry, "removed",
                           G_CALLBACK (on_app_info_removed),
                           request,
                           G_CONNECT_DEFAULT);

  if (!xdp_app_info_registry_contains (registry, request->app_info))
    {
      return dex_future_new_for_error (g_error_new_literal (G_IO_ERROR,
                                                            G_IO_ERROR_CANCELLED,
                                                            "Caller disconnected"));
    }

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (request),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request);

  if (request->impl_request)
    setup_impl_close_chain (request);

  return dex_future_new_for_object (request);
}

static DexFuture *
on_impl_request_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  RequestImplProxyCreateData *data = user_data;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;

  impl_request = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_request);

  return xdp_request_dex_new_inner (data->context,
                                    data->app_info,
                                    data->skeleton,
                                    impl_request,
                                    data->claim);
}

DexFuture *
xdp_request_dex_new (XdpContext             *context,
                     XdpAppInfo             *app_info,
                     GDBusInterfaceSkeleton *skeleton,
                     GDBusProxy             *proxy_impl,
                     GVariant               *arg_options)
{
  g_autoptr(XdpObjectPathClaim) claim = NULL;
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

  claim = xdp_context_claim_object_path (context, id);
  while (!claim)
    {
      uint32_t r = g_random_int ();
      g_free (id);
      id = g_strdup_printf (DESKTOP_DBUS_PATH "/request/%s/%s/%u",
                            sender,
                            token,
                            r);
      claim = xdp_context_claim_object_path (context, id);
    }

  if (proxy_impl)
    {
      g_autoptr(DexFuture) future = NULL;
      RequestImplProxyCreateData *data;

      future = xdp_dbus_impl_request_proxy_new_future (
        g_dbus_proxy_get_connection (proxy_impl),
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
        g_dbus_proxy_get_name (proxy_impl),
        id);

      data = g_new0 (RequestImplProxyCreateData, 1);
      data->context = context;
      data->app_info = g_object_ref (app_info);
      data->skeleton = g_object_ref (skeleton);
      data->claim = g_steal_pointer (&claim);

      future = dex_future_then (future,
                                on_impl_request_proxy_created,
                                g_steal_pointer (&data),
                                (GDestroyNotify) request_impl_proxy_create_data_free);

      return g_steal_pointer (&future);
    }

  return xdp_request_dex_new_inner (context, app_info, skeleton, NULL, claim);
}

gboolean
xdp_request_dex_export (XdpRequestDex  *request,
                        GError        **error)
{
  const char *path = xdp_request_dex_get_object_path (request);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         g_dbus_interface_skeleton_get_connection (request->skeleton),
                                         path,
                                         error))
    return FALSE;

  request->exported = TRUE;
  return TRUE;
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
  return xdp_object_path_claim_get_path (request->claim);
}
