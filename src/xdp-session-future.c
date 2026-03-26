/*
 * Copyright © 2025 Red Hat, Inc
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
 */

#include "config.h"

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#include "xdp-session-future.h"

enum
{
  SESSION_CLOSED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct _XdpSessionFuture
{
  XdpDbusSessionSkeleton parent_instance;

  XdpContext *context;
  XdpAppInfo *app_info;
  XdpDbusImplSession *impl_session;
  GDBusInterfaceSkeleton *skeleton;
  char *id;
  gboolean exported;
} XdpSessionFuture;

static void xdp_session_skeleton_iface_init (XdpDbusSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpSessionFuture,
                         xdp_session_future,
                         XDP_DBUS_TYPE_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SESSION,
                                                xdp_session_skeleton_iface_init))

static void
xdp_session_future_emit_closed (XdpSessionFuture *session)
{
  g_signal_emit (session, signals[SESSION_CLOSED], 0);
}

static void
xdp_session_future_on_signal_closed (XdpDbusSession *object,
                                     GVariant       *arg_details)
{
  XdpSessionFuture *session = XDP_SESSION_FUTURE (object);

  g_dbus_connection_emit_signal (g_dbus_interface_skeleton_get_connection (session->skeleton),
                                 xdp_app_info_get_sender (session->app_info),
                                 session->id,
                                 DESKTOP_DBUS_IFACE ".Session",
                                 "Closed",
                                 g_variant_new ("(@a{sv})", arg_details),
                                 NULL);
}

static gboolean
xdp_session_future_handle_close (XdpDbusSession        *object,
                                 GDBusMethodInvocation *invocation)
{
  XdpSessionFuture *session = XDP_SESSION_FUTURE (object);
  g_autoptr(GError) error = NULL;

  if (!session->exported)
    {
      xdp_dbus_session_complete_close (XDP_DBUS_SESSION (session), invocation);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  session->exported = FALSE;
  xdp_context_unclaim_object_path (session->context, session->id);

  dex_await (xdp_dbus_impl_session_call_close_future (session->impl_session),
             &error);

  xdp_session_future_emit_closed (session);

  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_session_complete_close (XDP_DBUS_SESSION (object), invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_session_skeleton_iface_init (XdpDbusSessionIface *iface)
{
  iface->handle_close = xdp_session_future_handle_close;
  iface->closed = xdp_session_future_on_signal_closed;
}

static void
xdp_session_future_dispose (GObject *object)
{
  XdpSessionFuture *session = XDP_SESSION_FUTURE (object);

  if (session->exported)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
      session->exported = FALSE;
      xdp_context_unclaim_object_path (session->context, session->id);

      xdp_dbus_impl_session_call_close (session->impl_session, NULL, NULL, NULL);
    }

  g_clear_object (&session->app_info);
  g_clear_object (&session->impl_session);
  g_clear_object (&session->skeleton);
  g_clear_pointer (&session->id, g_free);

  G_OBJECT_CLASS (xdp_session_future_parent_class)->dispose (object);
}

static void
xdp_session_future_init (XdpSessionFuture *session)
{
}

static void
xdp_session_future_class_init (XdpSessionFutureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_session_future_dispose;

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
  XdpSessionFuture *session = XDP_SESSION_FUTURE (user_data);

  if (g_strcmp0 (xdp_app_info_get_sender (session->app_info), peer) != 0)
    return;

  if (!session->exported)
    return;

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  session->exported = FALSE;
  xdp_context_unclaim_object_path (session->context, session->id);

  xdp_dbus_impl_session_call_close (session->impl_session, NULL, NULL, NULL);
  xdp_session_future_emit_closed (session);
}

static void
on_impl_closed (XdpDbusImplSession *object,
                gpointer            user_data)
{
  XdpSessionFuture *session = XDP_SESSION_FUTURE (user_data);
  g_auto(GVariantBuilder) details_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!session->exported)
    return;

  xdp_dbus_session_emit_closed (XDP_DBUS_SESSION (session),
                                g_variant_builder_end (&details_builder));

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  session->exported = FALSE;
  xdp_context_unclaim_object_path (session->context, session->id);

  xdp_session_future_emit_closed (session);
}

static gboolean
session_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  XdpSessionFuture *session = XDP_SESSION_FUTURE (user_data);
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
  GDBusInterfaceSkeleton *skeleton;
  char *id;
} SessionImplProxyCreateData;

static void
session_impl_proxy_create_data_free (SessionImplProxyCreateData *data)
{
  g_clear_object (&data->app_info);
  g_clear_object (&data->skeleton);
  g_clear_pointer (&data->id, g_free);
  free (data);
}

static DexFuture *
on_impl_session_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  SessionImplProxyCreateData *data = user_data;
  g_autoptr(XdpSessionFuture) session = NULL;
  g_autoptr(XdpDbusImplSession) impl_session = NULL;
  g_autoptr(GError) error = NULL;

  impl_session = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_session);

  session = g_object_new (XDP_TYPE_SESSION_FUTURE, NULL);
  session->context = g_steal_pointer (&data->context);
  session->app_info = g_steal_pointer (&data->app_info);
  session->impl_session = g_steal_pointer (&impl_session);
  session->skeleton = g_steal_pointer (&data->skeleton);
  session->id = g_steal_pointer (&data->id);
  session->exported = TRUE;

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
                                         g_dbus_interface_skeleton_get_connection (session->skeleton),
                                         session->id,
                                         &error))
      return dex_future_new_for_error (g_steal_pointer (&error));

  return dex_future_new_for_object (session);
}

DexFuture *
xdp_session_future_new (XdpContext             *context,
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
  data->skeleton = g_object_ref (skeleton);
  data->id = g_steal_pointer (&id);

  future = dex_future_then (future,
                            on_impl_session_proxy_created,
                            g_steal_pointer (&data),
                            (GDestroyNotify) session_impl_proxy_create_data_free);

  return g_steal_pointer (&future);
}

gboolean
xdp_session_future_is_closed (XdpSessionFuture *session)
{
  return !session->exported;
}

XdpAppInfo *
xdp_session_future_get_app_info (XdpSessionFuture *session)
{
  return session->app_info;
}

const char *
xdp_session_future_get_object_path (XdpSessionFuture *session)
{
  return session->id;
}

typedef struct _XdpSessionFutureStore
{
  GObject parent_instance;

  size_t session_offset;
  GHashTable *sessions; /* char *session_handle -> XdpSessionFuture *session */
} XdpSessionFutureStore;

G_DEFINE_FINAL_TYPE (XdpSessionFutureStore,
                     xdp_session_future_store,
                     G_TYPE_OBJECT)

static void
on_session_closed (XdpSessionFuture *session,
                   gpointer          user_data)
{
  XdpSessionFutureStore *store = XDP_SESSION_FUTURE_STORE (user_data);

  g_hash_table_remove (store->sessions,
                       xdp_session_future_get_object_path (session));
}

void
xdp_session_future_store_take_session (XdpSessionFutureStore *store,
                                       gpointer               session_wrapper)
{
  g_autoptr(GObject) owned_wrapper = G_OBJECT (session_wrapper);
  XdpSessionFuture *session;

  session = XDP_SESSION_FUTURE (G_STRUCT_MEMBER_P (owned_wrapper,
                                                   store->session_offset));

  if (!session || xdp_session_future_is_closed (session))
    return;

  g_signal_connect_object (session, "session-closed",
                           G_CALLBACK (on_session_closed),
                           store,
                           G_CONNECT_DEFAULT);

  g_hash_table_insert (store->sessions,
                       g_strdup (xdp_session_future_get_object_path (session)),
                       g_steal_pointer (&owned_wrapper));
}

gpointer
xdp_session_future_store_lookup_session (XdpSessionFutureStore *store,
                                         const char            *session_handle,
                                         XdpAppInfo            *app_info)
{
  GObject *session_wrapper =
    g_hash_table_lookup (store->sessions, session_handle);
  XdpSessionFuture *session;

  if (!session_wrapper)
    return NULL;

  session = XDP_SESSION_FUTURE (G_STRUCT_MEMBER_P (session_wrapper,
                                                   store->session_offset));

  if (app_info && xdp_session_future_get_app_info (session) != app_info)
    return NULL;

  return session_wrapper;
}

static void
xdp_session_future_store_dispose (GObject *object)
{
  XdpSessionFutureStore *store = XDP_SESSION_FUTURE_STORE (object);

  g_clear_pointer (&store->sessions, g_hash_table_unref);

  G_OBJECT_CLASS (xdp_session_future_store_parent_class)->dispose (object);
}

static void
xdp_session_future_store_init (XdpSessionFutureStore *store)
{
}

static void
xdp_session_future_store_class_init (XdpSessionFutureStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_session_future_store_dispose;
}

XdpSessionFutureStore *
xdp_session_future_store_new (size_t session_offset)
{
  XdpSessionFutureStore *store;

  store = g_object_new (XDP_TYPE_SESSION_FUTURE_STORE, NULL);
  store->session_offset = session_offset;
  store->sessions =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free,
                           (GDestroyNotify) g_object_unref);

  return store;
}
