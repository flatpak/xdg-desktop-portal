/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-session-dex.h"

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

  XdpContext *context;
  XdpAppInfo *app_info;
  XdpDbusImplSession *impl_session;
  GDBusConnection *connection;
  char *id;
  gboolean exported;
  gboolean claimed;
  gboolean closed_emitted;
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
  if (session->closed_emitted)
    return;

  session->closed_emitted = TRUE;
  g_signal_emit (session, signals[SESSION_CLOSED], 0);
}

static void
xdp_session_dex_release_path (XdpSessionDex *session)
{
  if (!session->claimed)
    return;

  xdp_context_unclaim_object_path (session->context, session->id);
  session->claimed = FALSE;
}

static gboolean
xdp_session_dex_close_local (XdpSessionDex *session)
{
  if (!session->exported)
    return FALSE;

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  session->exported = FALSE;
  xdp_session_dex_release_path (session);
  xdp_session_dex_emit_closed (session);
  return TRUE;
}

void
xdp_session_dex_close (XdpSessionDex *session)
{
  g_return_if_fail (XDP_IS_SESSION_DEX (session));

  if (xdp_session_dex_close_local (session))
    xdp_dbus_impl_session_call_close (session->impl_session, NULL, NULL, NULL);
}

static void
xdp_session_dex_on_signal_closed (XdpDbusSession *object,
                                  GVariant       *arg_details)
{
  XdpSessionDex *session = XDP_SESSION_DEX (object);

  if (!session->connection)
    return;

  g_dbus_connection_emit_signal (session->connection,
                                 xdp_app_info_get_sender (session->app_info),
                                 session->id,
                                 DESKTOP_DBUS_IFACE ".Session",
                                 "Closed",
                                 g_variant_new ("(@a{sv})", arg_details),
                                 NULL);
}

static gboolean
xdp_session_dex_handle_close (XdpDbusSession        *object,
                              GDBusMethodInvocation *invocation)
{
  g_autoptr(XdpSessionDex) session = g_object_ref (XDP_SESSION_DEX (object));
  g_autoptr(GError) error = NULL;

  if (!xdp_session_dex_close_local (session))
    {
      xdp_dbus_session_complete_close (XDP_DBUS_SESSION (session), invocation);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  dex_await (xdp_dbus_impl_session_call_close_future (session->impl_session),
             &error);

  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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

  if (session->exported)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
      session->exported = FALSE;

      xdp_dbus_impl_session_call_close (session->impl_session, NULL, NULL, NULL);
    }

  xdp_session_dex_release_path (session);

  g_clear_object (&session->app_info);
  g_clear_object (&session->impl_session);
  g_clear_object (&session->connection);
  g_clear_pointer (&session->id, g_free);

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
on_peer_disconnect (XdpContext *context,
                    const char *peer,
                    gpointer    user_data)
{
  g_autoptr(XdpSessionDex) session = g_object_ref (XDP_SESSION_DEX (user_data));

  if (g_strcmp0 (xdp_app_info_get_sender (session->app_info), peer) != 0)
    return;

  if (!session->exported)
    return;

  xdp_session_dex_close (session);
}

static void
on_impl_closed (XdpDbusImplSession *object,
                gpointer            user_data)
{
  g_autoptr(XdpSessionDex) session = g_object_ref (XDP_SESSION_DEX (user_data));
  g_auto(GVariantBuilder) details_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!session->exported)
    return;

  xdp_dbus_session_emit_closed (XDP_DBUS_SESSION (session),
                                g_variant_builder_end (&details_builder));

  xdp_session_dex_close_local (session);
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

typedef struct _SessionImplProxyCreateData {
  XdpContext *context;
  XdpAppInfo *app_info;
  GDBusConnection *connection;
  char *id;
  char *peer;
  gulong peer_disconnect_handler;
  gboolean peer_disconnected;
} SessionImplProxyCreateData;

static void
on_pending_peer_disconnect (XdpContext *context,
                            const char *peer,
                            gpointer    user_data)
{
  SessionImplProxyCreateData *data = user_data;

  if (g_strcmp0 (data->peer, peer) == 0)
    data->peer_disconnected = TRUE;
}

static void
session_impl_proxy_create_data_free (SessionImplProxyCreateData *data)
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

static DexFuture *
on_impl_session_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  SessionImplProxyCreateData *data = user_data;
  g_autoptr(XdpSessionDex) session = NULL;
  g_autoptr(XdpDbusImplSession) impl_session = NULL;
  g_autoptr(GError) error = NULL;

  if (data->peer_disconnected)
    return dex_future_new_for_error (
      g_error_new_literal (G_IO_ERROR,
                           G_IO_ERROR_CANCELLED,
                           "Caller disconnected"));

  impl_session = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_session);

  session = g_object_new (XDP_TYPE_SESSION_DEX, NULL);
  session->context = g_steal_pointer (&data->context);
  session->app_info = g_steal_pointer (&data->app_info);
  session->impl_session = g_steal_pointer (&impl_session);
  session->connection = g_steal_pointer (&data->connection);
  session->id = g_steal_pointer (&data->id);
  session->claimed = TRUE;

  g_signal_connect_object (session->context, "peer-disconnect",
                           G_CALLBACK (on_peer_disconnect),
                           session,
                           G_CONNECT_DEFAULT);

  g_signal_connect_object (session->impl_session, "closed",
                           G_CALLBACK (on_impl_closed),
                           session,
                           G_CONNECT_DEFAULT);

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (session),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (session, "g-authorize-method",
                    G_CALLBACK (session_authorize_callback),
                    session);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                          session->connection,
                                          session->id,
                                          &error))
    {
      g_clear_signal_handler (&data->peer_disconnect_handler,
                              session->context);
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  session->exported = TRUE;
  g_clear_signal_handler (&data->peer_disconnect_handler, session->context);
  if (data->peer_disconnected)
    {
      xdp_session_dex_close (session);
      return dex_future_new_for_error (
        g_error_new_literal (G_IO_ERROR,
                             G_IO_ERROR_CANCELLED,
                             "Caller disconnected"));
    }

  return dex_future_new_for_object (session);
}

DexFuture *
xdp_session_dex_new (XdpContext             *context,
                     XdpAppInfo             *app_info,
                     GDBusInterfaceSkeleton *skeleton,
                     GDBusProxy             *proxy_impl,
                     GVariant               *arg_options)
{
  g_autoptr(DexFuture) future = NULL;
  SessionImplProxyCreateData *data;
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

  while (!xdp_context_claim_object_path (context, id))
    {
      uint32_t r = g_random_int ();
      g_free (id);
      id = g_strdup_printf (DESKTOP_DBUS_PATH "/session/%s/%s/%u",
                            sender,
                            token,
                            r);
    }

  future = xdp_dbus_impl_session_proxy_new_future (
    g_dbus_proxy_get_connection (proxy_impl),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (proxy_impl),
    id);

  data = g_new0 (SessionImplProxyCreateData, 1);
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
                            on_impl_session_proxy_created,
                            g_steal_pointer (&data),
                            (GDestroyNotify) session_impl_proxy_create_data_free);

  return g_steal_pointer (&future);
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
  return session->id;
}

typedef struct _XdpSessionDexStore
{
  GObject parent_instance;

  size_t session_offset;
  GHashTable *sessions; /* char *session_handle -> GObject *session_wrapper */
} XdpSessionDexStore;

G_DEFINE_FINAL_TYPE (XdpSessionDexStore,
                     xdp_session_dex_store,
                     G_TYPE_OBJECT)

static XdpSessionDex *
session_from_wrapper (XdpSessionDexStore *store,
                      GObject            *session_wrapper)
{
  if (store->session_offset == G_MAXSIZE)
    return XDP_SESSION_DEX (session_wrapper);

  return G_STRUCT_MEMBER (XdpSessionDex *,
                          session_wrapper,
                          store->session_offset);
}

static void
on_session_closed (XdpSessionDex *session,
                   gpointer       user_data)
{
  XdpSessionDexStore *store = XDP_SESSION_DEX_STORE (user_data);
  GObject *session_wrapper;

  if (store->sessions == NULL)
    return;

  session_wrapper = g_hash_table_lookup (
    store->sessions,
    xdp_session_dex_get_object_path (session));

  if (session_wrapper && session_from_wrapper (store, session_wrapper) == session)
    g_hash_table_remove (store->sessions,
                         xdp_session_dex_get_object_path (session));
}

void
xdp_session_dex_store_take_session (XdpSessionDexStore *store,
                                    gpointer            session_wrapper)
{
  g_autoptr(GObject) owned_wrapper = G_OBJECT (session_wrapper);
  XdpSessionDex *session;

  session = session_from_wrapper (store, owned_wrapper);

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

  session = session_from_wrapper (store, session_wrapper);

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
  return xdp_session_dex_store_new_with_offset (G_MAXSIZE);
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
