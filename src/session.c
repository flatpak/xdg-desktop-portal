/*
 * Copyright © 2017 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "session.h"
#include "request.h"
#include "call.h"

#include <string.h>

enum
{
  PROP_0,

  PROP_SENDER,
  PROP_APP_ID,
  PROP_TOKEN,
  PROP_CONNECTION,
  PROP_IMPL_CONNECTION,
  PROP_IMPL_DBUS_NAME,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

G_LOCK_DEFINE (sessions);
static GHashTable *sessions;

static void g_initable_iface_init (GInitableIface *iface);
static void session_skeleton_iface_init (XdpSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (Session, session, XDP_TYPE_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SESSION,
                                                session_skeleton_iface_init))

#define SESSION_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS ((o), session_get_type (), SessionClass))

const char *
lookup_session_token (GVariant *options)
{
  const char *token = NULL;

  g_variant_lookup (options, "session_handle_token", "&s", &token);

  return token;
}

Session *
acquire_session (const char *session_handle,
                 Request *request)
{
  g_autoptr(Session) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  if (!session)
    return NULL;

  if (g_strcmp0 (session->sender, request->sender) != 0)
    return NULL;

  if (g_strcmp0 (session->app_id, xdp_app_info_get_id (request->app_info)) != 0)
    return NULL;

  return g_steal_pointer (&session);
}

Session *
acquire_session_from_call (const char *session_handle,
                           Call *call)
{
  g_autoptr(Session) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  if (!session)
    return NULL;

  if (g_strcmp0 (session->sender, call->sender) != 0)
    return NULL;

  if (g_strcmp0 (session->app_id, xdp_app_info_get_id (call->app_info)) != 0)
    return NULL;

  return g_steal_pointer (&session);
}

Session *
lookup_session (const char *session_handle)
{
  g_autoptr(Session) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  return g_steal_pointer (&session);
}

gboolean
session_export (Session *session,
                GError **error)
{
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         session->connection,
                                         session->id,
                                         error))
    return FALSE;

  g_object_ref (session);
  session->exported = TRUE;

  return TRUE;
}

static void
session_unexport (Session *session)
{
  session->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  g_object_unref (session);
}

void
session_register (Session *session)
{
  G_LOCK (sessions);
  g_hash_table_insert (sessions, session->id, session);
  G_UNLOCK (sessions);
}

static void
session_unregister (Session *session)
{
  G_LOCK (sessions);
  g_hash_table_remove (sessions, session->id);
  G_UNLOCK (sessions);
}

void
session_close (Session *session,
               gboolean notify_closed)
{
  if (session->closed)
    return;

  SESSION_GET_CLASS (session)->close (session);

  if (notify_closed)
    {
      GVariantBuilder details_builder;

      g_variant_builder_init (&details_builder, G_VARIANT_TYPE_VARDICT);
      g_signal_emit_by_name (session, "closed",
                             g_variant_builder_end (&details_builder));
    }

  if (session->exported)
    session_unexport (session);

  session_unregister (session);

  if (session->impl_session)
    {
      g_autoptr(GError) error = NULL;

      if (!xdp_impl_session_call_close_sync (session->impl_session,
                                             NULL, &error))
        g_warning ("Failed to close session implementation: %s",
                   error->message);

      g_clear_object (&session->impl_session);
    }

  session->closed = TRUE;


  g_object_unref (session);
}

static gboolean
handle_close (XdpSession *object,
              GDBusMethodInvocation *invocation)
{
  Session *session = (Session *)object;

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  session_close (session, FALSE);

  xdp_session_complete_close (object, invocation);

  return TRUE;
}

static void
session_skeleton_iface_init (XdpSessionIface *iface)
{
  iface->handle_close = handle_close;
}

static void
close_sessions_in_thread_func (GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  const char *sender = (const char *)task_data;
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  Session *session;

  G_LOCK (sessions);
  if (sessions)
    {
      g_hash_table_iter_init (&iter, sessions);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&session))
        {
          if (strcmp (sender, session->sender) == 0)
            list = g_slist_prepend (list, g_object_ref (session));
        }
    }
  G_UNLOCK (sessions);

  for (l = list; l; l = l->next)
    {
      Session *session = l->data;

      SESSION_AUTOLOCK (session);
      session_close (session, FALSE);
    }

  g_slist_free_full (list, g_object_unref);
}

void
close_sessions_for_sender (const char *sender)
{
  g_autoptr(GTask) task = NULL;

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_strdup (sender), g_free);
  g_task_run_in_thread (task, close_sessions_in_thread_func);
}

static void
on_closed (XdpImplSession *object, GObject *data)
{
  Session *session = (Session *)data;

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  g_clear_object (&session->impl_session);
  session_close (session, TRUE);
}

static gboolean
session_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  const gchar *session_owner = user_data;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (strcmp (sender, session_owner) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed, Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

static gboolean
session_initable_init (GInitable *initable,
                       GCancellable *cancellable,
                       GError **error)
{
  Session *session = (Session *)initable;
  g_autofree char *sender_escaped = NULL;
  g_autofree char *id = NULL;
  g_autoptr(XdpImplSession) impl_session = NULL;
  int i;

  sender_escaped = g_strdup (session->sender + 1);
  for (i = 0; sender_escaped[i]; i++)
    {
      if (sender_escaped[i] == '.')
        sender_escaped[i] = '_';
    }

  if (!session->token)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Missing token");
      return FALSE;
    }

  id = g_strdup_printf ("/org/freedesktop/portal/desktop/session/%s/%s",
                        sender_escaped, session->token);

  if (session->impl_dbus_name)
    {
      impl_session = xdp_impl_session_proxy_new_sync (session->impl_connection,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      session->impl_dbus_name,
                                                      id,
                                                      NULL, error);
      if (!impl_session)
        return FALSE;

      g_signal_connect (impl_session, "closed", G_CALLBACK (on_closed), session);

      session->impl_session = g_steal_pointer (&impl_session);
    }

  session->id = g_steal_pointer (&id);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (session),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (session, "g-authorize-method",
                    G_CALLBACK (session_authorize_callback),
                    session->sender);

  return TRUE;
}

static void
g_initable_iface_init (GInitableIface *iface)
{
  iface->init = session_initable_init;
}

static void
session_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  Session *session = (Session *)object;

  switch (prop_id)
    {
    case PROP_SENDER:
      session->sender = g_strdup (g_value_get_string (value));
      break;

    case PROP_APP_ID:
      session->app_id = g_strdup (g_value_get_string (value));
      break;

    case PROP_TOKEN:
      session->token = g_strdup (g_value_get_string (value));
      break;

    case PROP_CONNECTION:
      g_set_object (&session->connection, g_value_get_object (value));
      break;

    case PROP_IMPL_CONNECTION:
      g_set_object (&session->impl_connection, g_value_get_object (value));
      break;

    case PROP_IMPL_DBUS_NAME:
      session->impl_dbus_name = g_strdup (g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
session_get_property (GObject *object,
                      guint prop_id,
                      GValue *value,
                      GParamSpec *pspec)
{
  Session *session = (Session *)object;

  switch (prop_id)
    {
    case PROP_SENDER:
      g_value_set_string (value, session->sender);
      break;

    case PROP_APP_ID:
      g_value_set_string (value, session->app_id);
      break;

    case PROP_TOKEN:
      g_value_set_string (value, session->token);
      break;

    case PROP_CONNECTION:
      g_value_set_object (value, session->connection);
      break;

    case PROP_IMPL_CONNECTION:
      g_value_set_object (value, session->impl_connection);
      break;

    case PROP_IMPL_DBUS_NAME:
      g_value_set_string (value, session->impl_dbus_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
session_finalize (GObject *object)
{
  Session *session = (Session *)object;

  g_assert (!session->id || !g_hash_table_lookup (sessions, session->id));

  g_free (session->sender);
  g_clear_object (&session->connection);

  g_clear_object (&session->impl_connection);
  g_free (session->impl_dbus_name);
  g_clear_object (&session->impl_session);

  g_free (session->app_id);
  g_free (session->id);

  g_mutex_clear (&session->mutex);

  G_OBJECT_CLASS (session_parent_class)->finalize (object);
}

static void
session_init (Session *session)
{
  g_mutex_init (&session->mutex);
}

static void
session_class_init (SessionClass *klass)
{
  GObjectClass *gobject_class;

  sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = session_finalize;
  gobject_class->set_property = session_set_property;
  gobject_class->get_property = session_get_property;

  obj_props[PROP_SENDER] =
    g_param_spec_string ("sender", "Sender", "Sender",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_APP_ID] =
    g_param_spec_string ("app-id", "app-id", "App ID",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_TOKEN] =
    g_param_spec_string ("token", "token", "Token",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_CONNECTION] =
    g_param_spec_object ("connection", "connection",
                         "DBus connection",
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_IMPL_CONNECTION] =
    g_param_spec_object ("impl-connection", "impl-connection",
                         "impl DBus connection",
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_IMPL_DBUS_NAME] =
    g_param_spec_string ("impl-dbus-name", "impl-dbus-name",
                         "impl DBus name",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);
}
