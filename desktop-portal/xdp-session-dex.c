/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-session-dex.h"

#include "xdp-app-info-registry.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

enum
{
  SESSION_CLOSED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct _XdpSessionDex
{
  XdpDbusSessionSkeleton parent_instance;

  XdpContext *context; /* (unowned) (not nullable) */
  XdpAppInfo *app_info; /* (owned) (not nullable) */
  XdpDbusImplSession *impl_session; /* (owned) (nullable) */
  GDBusInterfaceSkeleton *skeleton; /* (owned) (not nullable) */
  XdpObjectPathClaim *claim; /* (owned) (not nullable) */
  DexPromise *close_promise; /* (owned) (not nullable) */
  DexPromise *cancel_promise; /* (owned) (not nullable) */
  gboolean exported;
} XdpSessionDex;

static void xdp_session_skeleton_iface_init (XdpDbusSessionIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSessionDex,
                               xdp_session_dex,
                               XDP_DBUS_TYPE_SESSION_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SESSION,
                                                      xdp_session_skeleton_iface_init))

static void
xdp_session_dex_emit_closed (XdpSessionDex *session)
{
  g_signal_emit (session, signals[SESSION_CLOSED], 0);
}

static void
xdp_session_dex_on_signal_closed (XdpDbusSession *object,
                                  GVariant       *arg_details)
{
  XdpSessionDex *session = XDP_SESSION_DEX (object);
  GDBusConnection *connection;

  connection = g_dbus_interface_skeleton_get_connection (session->skeleton);
  if (!connection)
    return;

  g_dbus_connection_emit_signal (connection,
                                 xdp_app_info_get_sender (session->app_info),
                                 xdp_session_dex_get_object_path (session),
                                 DESKTOP_DBUS_IFACE ".Session",
                                 "Closed",
                                 g_variant_new ("(@a{sv})", arg_details),
                                 NULL);
}

void
xdp_session_dex_close (XdpSessionDex *session)
{
  if (session->exported)
    {
      session->exported = FALSE;
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
    }

  if (!dex_future_is_resolved (DEX_FUTURE (session->close_promise)))
    dex_promise_resolve_boolean (session->close_promise, TRUE);
}

static gboolean
xdp_session_dex_handle_close (XdpDbusSession        *object,
                              GDBusMethodInvocation *invocation)
{
  XdpSessionDex *session = XDP_SESSION_DEX (object);

  if (session->exported)
    {
      xdp_session_dex_close (session);
      xdp_session_dex_emit_closed (session);
    }

  xdp_dbus_session_complete_close (XDP_DBUS_SESSION (session), invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_session_skeleton_iface_init (XdpDbusSessionIface *iface)
{
  iface->handle_close = xdp_session_dex_handle_close;
  iface->closed = xdp_session_dex_on_signal_closed;
}

static void
xdp_session_dex_dispose (GObject *object)
{
  XdpSessionDex *session = XDP_SESSION_DEX (object);

  xdp_session_dex_close (session);

  if (!dex_future_is_resolved (DEX_FUTURE (session->cancel_promise)))
    dex_promise_resolve_boolean (session->cancel_promise, TRUE);

  dex_clear (&session->close_promise);
  dex_clear (&session->cancel_promise);
  g_clear_pointer (&session->claim, xdp_object_path_claim_unref);
  g_clear_object (&session->app_info);
  g_clear_object (&session->impl_session);
  g_clear_object (&session->skeleton);

  G_OBJECT_CLASS (xdp_session_dex_parent_class)->dispose (object);
}

static void
xdp_session_dex_init (XdpSessionDex *session)
{
}

static void
xdp_session_dex_class_init (XdpSessionDexClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_session_dex_dispose;

  signals[SESSION_CLOSED] =
    g_signal_new ("session-closed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
on_app_info_removed (XdpAppInfoRegistry *registry,
                     XdpAppInfo         *app_info,
                     gpointer            user_data)
{
  XdpSessionDex *session = XDP_SESSION_DEX (user_data);

  if (app_info != session->app_info)
    return;

  if (!session->exported)
    return;

  xdp_session_dex_close (session);
  xdp_session_dex_emit_closed (session);
}

static void
on_impl_closed (XdpDbusImplSession *object,
                gpointer            user_data)
{
  XdpSessionDex *session = XDP_SESSION_DEX (user_data);
  g_auto(GVariantBuilder) details_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!session->exported)
    return;

  xdp_dbus_session_emit_closed (XDP_DBUS_SESSION (session),
                                g_variant_builder_end (&details_builder));

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  session->exported = FALSE;

  xdp_session_dex_emit_closed (session);
}

static gboolean
session_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  XdpSessionDex *session = XDP_SESSION_DEX (user_data);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_strcmp0 (sender, xdp_app_info_get_sender (session->app_info)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed, Unmatched caller");
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

#define IMPL_SESSION_POLL_INITIAL_DELAY_MS 100
#define IMPL_SESSION_POLL_MAX_TOTAL_MS 30000

static DexFuture *
check_impl_session_available (DexFuture *completed,
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
                                                  "org.freedesktop.impl.portal.Session"),
                                   G_VARIANT_TYPE ("(a{sv})"),
                                   G_DBUS_CALL_FLAGS_NONE, -1);
}

static DexFuture *
retry_check_impl_session_poll (DexFuture *completed,
                               gpointer   user_data)
{
  PollData *pd = user_data;
  g_autoptr(GError) error = NULL;
  unsigned int delay_ms;

  dex_future_get_value (completed, &error);

  if (!g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) &&
      !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT))
    return dex_future_new_for_error (g_steal_pointer (&error));

  if (pd->poll_total_ms >= IMPL_SESSION_POLL_MAX_TOTAL_MS)
    {
      g_warning ("Impl session at %s did not appear after %ums",
                 g_dbus_proxy_get_object_path (pd->proxy),
                 pd->poll_total_ms);
      return dex_future_new_for_boolean (TRUE);
    }

  delay_ms = MIN (pd->poll_delay_ms,
                  IMPL_SESSION_POLL_MAX_TOTAL_MS - pd->poll_total_ms);
  pd->poll_total_ms += delay_ms;
  pd->poll_delay_ms = pd->poll_delay_ms * 2;

  g_debug ("Impl session at %s does not exist yet, retrying in %ums",
           g_dbus_proxy_get_object_path (pd->proxy),
           delay_ms);

  return dex_future_catch (dex_timeout_new_usec (delay_ms * 1000),
                           check_impl_session_available,
                           user_data, NULL);
}

static DexFuture *
do_impl_session_close (DexFuture *completed,
                       gpointer   user_data)
{
  XdpDbusImplSession *impl_session = XDP_DBUS_IMPL_SESSION (user_data);

  return xdp_dbus_impl_session_call_close_future (impl_session);
}

static void
setup_impl_close_chain (XdpSessionDex *session)
{
  PollData *pd;
  DexFuture *signal;
  DexFuture *poll;
  DexFuture *impl_discovery;
  DexFuture *close_trigger;
  DexFuture *close_chain;

  pd = g_new0 (PollData, 1);
  pd->proxy = g_object_ref (G_DBUS_PROXY (session->impl_session));
  pd->poll_delay_ms = IMPL_SESSION_POLL_INITIAL_DELAY_MS;
  pd->poll_total_ms = 0;

  /* The Created signal (v2) reliably detects the impl session. */
  signal = xdp_dbus_impl_session_wait_created_future (session->impl_session);

  /* The poll fallback (v1) can miss the impl session if the backend
   * exports and unexports it between polls. That is acceptable
   * because we can only miss it if it already went away on its own,
   * so there is nothing left to close. */
  poll = dex_future_catch_loop (check_impl_session_available (NULL, pd),
                                retry_check_impl_session_poll,
                                pd,
                                (GDestroyNotify) poll_data_free);

  impl_discovery = dex_future_any (signal, poll,
                                   DEX_FUTURE (dex_ref (session->cancel_promise)),
                                   NULL);

  close_trigger = dex_future_all (
      DEX_FUTURE (dex_ref (session->close_promise)),
      impl_discovery,
      NULL);

  close_chain = dex_future_then (close_trigger,
                                 do_impl_session_close,
                                 g_object_ref (session->impl_session),
                                 g_object_unref);

  dex_future_disown (close_chain);
}

typedef struct _SessionImplProxyCreateData {
  XdpContext *context;
  XdpAppInfo *app_info;
  GDBusInterfaceSkeleton *skeleton;
  XdpObjectPathClaim *claim;
} SessionImplProxyCreateData;

static void
session_impl_proxy_create_data_free (SessionImplProxyCreateData *data)
{
  g_clear_pointer (&data->claim, xdp_object_path_claim_unref);
  g_clear_object (&data->app_info);
  g_clear_object (&data->skeleton);
  g_free (data);
}

static DexFuture *
xdp_session_dex_new_inner (XdpContext             *context,
                           XdpAppInfo             *app_info,
                           GDBusInterfaceSkeleton *skeleton,
                           XdpDbusImplSession     *impl_session,
                           XdpObjectPathClaim     *claim)
{
  XdpAppInfoRegistry *registry =
    xdp_context_get_app_info_registry (context);
  g_autoptr(XdpSessionDex) session = NULL;
  g_autoptr(GError) error = NULL;
  const char *path;

  session = g_object_new (XDP_TYPE_SESSION_DEX, NULL);
  session->context = context;
  session->app_info = g_object_ref (app_info);
  session->skeleton = g_object_ref (skeleton);
  session->claim = xdp_object_path_claim_ref (claim);
  session->close_promise = dex_promise_new ();
  session->cancel_promise = dex_promise_new ();

  if (impl_session)
    session->impl_session = g_object_ref (impl_session);

  g_signal_connect_object (registry, "removed",
                           G_CALLBACK (on_app_info_removed),
                           session,
                           G_CONNECT_DEFAULT);

  if (!xdp_app_info_registry_contains (registry, session->app_info))
    {
      return dex_future_new_for_error (
        g_error_new_literal (G_IO_ERROR,
                             G_IO_ERROR_CANCELLED,
                             "Caller disconnected"));
    }

  if (session->impl_session)
    {
      g_signal_connect_object (session->impl_session, "closed",
                               G_CALLBACK (on_impl_closed),
                               session,
                               G_CONNECT_DEFAULT);

      setup_impl_close_chain (session);
    }

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (session),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (session, "g-authorize-method",
                    G_CALLBACK (session_authorize_callback),
                    session);

  path = xdp_session_dex_get_object_path (session);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         g_dbus_interface_skeleton_get_connection (session->skeleton),
                                         path,
                                         &error))
      return dex_future_new_for_error (g_steal_pointer (&error));

  session->exported = TRUE;

  return dex_future_new_for_object (session);
}

static DexFuture *
on_impl_session_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  SessionImplProxyCreateData *data = user_data;
  g_autoptr(XdpDbusImplSession) impl_session = NULL;

  impl_session = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_session);

  return xdp_session_dex_new_inner (data->context,
                                    data->app_info,
                                    data->skeleton,
                                    impl_session,
                                    data->claim);
}

DexFuture *
xdp_session_dex_new (XdpContext             *context,
                     XdpAppInfo             *app_info,
                     GDBusInterfaceSkeleton *skeleton,
                     GDBusProxy             *proxy_impl,
                     GVariant               *arg_options)
{
  g_autoptr(XdpObjectPathClaim) claim = NULL;
  const char *token = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *id = NULL;

  g_variant_lookup (arg_options, "session_handle_token", "&s", &token);
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

  id = g_strdup_printf (DESKTOP_DBUS_PATH "/session/%s/%s", sender, token);

  claim = xdp_context_claim_object_path (context, id);
  while (!claim)
    {
      uint32_t r = g_random_int ();
      g_free (id);
      id = g_strdup_printf (DESKTOP_DBUS_PATH "/session/%s/%s/%u",
                            sender,
                            token,
                            r);
      claim = xdp_context_claim_object_path (context, id);
    }

  if (proxy_impl)
    {
      g_autoptr(DexFuture) future = NULL;
      SessionImplProxyCreateData *data;

      future = xdp_dbus_impl_session_proxy_new_future (
        g_dbus_proxy_get_connection (proxy_impl),
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
        g_dbus_proxy_get_name (proxy_impl),
        id);

      data = g_new0 (SessionImplProxyCreateData, 1);
      data->context = context;
      data->app_info = g_object_ref (app_info);
      data->skeleton = g_object_ref (skeleton);
      data->claim = g_steal_pointer (&claim);

      future = dex_future_then (future,
                                on_impl_session_proxy_created,
                                g_steal_pointer (&data),
                                (GDestroyNotify) session_impl_proxy_create_data_free);

      return g_steal_pointer (&future);
    }

  return xdp_session_dex_new_inner (context, app_info, skeleton, NULL, claim);
}

gboolean
xdp_session_dex_is_closed (XdpSessionDex *session)
{
  return !session->exported;
}

XdpAppInfo *
xdp_session_dex_get_app_info (XdpSessionDex *session)
{
  return session->app_info;
}

const char *
xdp_session_dex_get_object_path (XdpSessionDex *session)
{
  return xdp_object_path_claim_get_path (session->claim);
}

typedef struct _XdpSessionDexStore
{
  GObject parent_instance;

  size_t session_offset;
  GHashTable *sessions; /* char *session_handle -> XdpSessionDex *session */
} XdpSessionDexStore;

G_DEFINE_FINAL_TYPE (XdpSessionDexStore,
                     xdp_session_dex_store,
                     G_TYPE_OBJECT)

static void
on_session_closed (XdpSessionDex *session,
                   gpointer       user_data)
{
  XdpSessionDexStore *store = XDP_SESSION_DEX_STORE (user_data);

  g_hash_table_remove (store->sessions,
                       xdp_session_dex_get_object_path (session));
}

void
xdp_session_dex_store_take_session (XdpSessionDexStore *store,
                                    gpointer            session_wrapper)
{
  g_autoptr(GObject) owned_wrapper = G_OBJECT (session_wrapper);
  XdpSessionDex *session;

  session = XDP_SESSION_DEX (G_STRUCT_MEMBER_P (owned_wrapper,
                                                store->session_offset));

  if (!session || xdp_session_dex_is_closed (session))
    return;

  g_signal_connect_object (session, "session-closed",
                           G_CALLBACK (on_session_closed),
                           store,
                           G_CONNECT_DEFAULT);

  g_hash_table_insert (store->sessions,
                       g_strdup (xdp_session_dex_get_object_path (session)),
                       g_steal_pointer (&owned_wrapper));
}

gpointer
xdp_session_dex_store_lookup_session (XdpSessionDexStore *store,
                                      const char         *session_handle,
                                      XdpAppInfo         *app_info)
{
  GObject *session_wrapper =
    g_hash_table_lookup (store->sessions, session_handle);
  XdpSessionDex *session;

  if (!session_wrapper)
    return NULL;

  session = XDP_SESSION_DEX (G_STRUCT_MEMBER_P (session_wrapper,
                                                store->session_offset));

  if (app_info && xdp_session_dex_get_app_info (session) != app_info)
    return NULL;

  return session_wrapper;
}

static void
xdp_session_dex_store_dispose (GObject *object)
{
  XdpSessionDexStore *store = XDP_SESSION_DEX_STORE (object);

  g_clear_pointer (&store->sessions, g_hash_table_unref);

  G_OBJECT_CLASS (xdp_session_dex_store_parent_class)->dispose (object);
}

static void
xdp_session_dex_store_init (XdpSessionDexStore *store)
{
}

static void
xdp_session_dex_store_class_init (XdpSessionDexStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_session_dex_store_dispose;
}

XdpSessionDexStore *
xdp_session_dex_store_new (void)
{
  return xdp_session_dex_store_new_with_offset (0);
}

XdpSessionDexStore *
xdp_session_dex_store_new_with_offset (size_t session_offset)
{
  XdpSessionDexStore *store;

  store = g_object_new (XDP_TYPE_SESSION_DEX_STORE, NULL);
  store->session_offset = session_offset;
  store->sessions =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free,
                           (GDestroyNotify) g_object_unref);

  return store;
}
