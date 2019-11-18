
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

enum
{
  PROP_0,

  PROP_ID,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

static GHashTable *sessions;

static void session_skeleton_iface_init (XdpImplSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (Session, session, XDP_IMPL_TYPE_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_IMPL_TYPE_SESSION,
                                                session_skeleton_iface_init))

#define SESSION_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS ((o), session_get_type (), SessionClass))

Session *
lookup_session (const char *id)
{
  return g_hash_table_lookup (sessions, id);
}

gboolean
session_export (Session *session,
                GDBusConnection *connection,
                GError **error)
{
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         connection,
                                         session->id,
                                         error))
    return FALSE;

  g_object_ref (session);
  session->exported = TRUE;

  return TRUE;
}

void
session_unexport (Session *session)
{
  session->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  g_object_unref (session);
}

void
session_close (Session *session)
{
  if (session->exported)
    session_unexport (session);

  session->closed = TRUE;

  SESSION_GET_CLASS (session)->close (session);

  g_object_unref (session);
}

static gboolean
handle_close (XdpImplSession *object,
              GDBusMethodInvocation *invocation)
{
  Session *session = (Session *)object;

  if (!session->closed)
    session_close (session);

  xdp_impl_session_complete_close (object, invocation);

  return TRUE;
}

static void
session_skeleton_iface_init (XdpImplSessionIface *iface)
{
  iface->handle_close = handle_close;
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
    case PROP_ID:
      session->id = g_strdup (g_value_get_string (value));
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
    case PROP_ID:
      g_value_set_string (value, session->id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
session_finalize (GObject *object)
{
  Session *session = (Session *)object;

  g_hash_table_remove (sessions, session->id);

  g_free (session->id);

  G_OBJECT_CLASS (session_parent_class)->finalize (object);
}

static void
session_constructed (GObject *object)
{
  Session *session = (Session *)object;

  g_hash_table_insert (sessions, g_strdup (session->id), session);

  G_OBJECT_CLASS (session_parent_class)->constructed (object);
}

static void
session_init (Session *session)
{
}

static void
session_class_init (SessionClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->constructed = session_constructed;
  gobject_class->finalize = session_finalize;
  gobject_class->set_property = session_set_property;
  gobject_class->get_property = session_get_property;

  obj_props[PROP_ID] =
    g_param_spec_string ("id", "id", "ID",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);

  sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    g_free, NULL);
}
