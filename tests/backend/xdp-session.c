
/*
 * Copyright © 2017 Red Hat, Inc
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
 *
 */

#include "xdp-session.h"

enum
{
  PROP_0,

  PROP_ID,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

static GHashTable *sessions;

static void xdp_session_skeleton_iface_init (XdpDbusImplSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpSession, xdp_session, XDP_DBUS_IMPL_TYPE_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_IMPL_TYPE_SESSION,
                                                xdp_session_skeleton_iface_init))

#define XDP_SESSION_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS ((o), xdp_session_get_type (), XdpSessionClass))

XdpSession *
lookup_session (const char *id)
{
  return g_hash_table_lookup (sessions, id);
}

gboolean
xdp_session_export (XdpSession       *session,
                    GDBusConnection  *connection,
                    GError          **error)
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
xdp_session_unexport (XdpSession *session)
{
  session->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  g_object_unref (session);
}

void
xdp_session_close (XdpSession *session)
{
  if (session->exported)
    xdp_session_unexport (session);

  session->closed = TRUE;

  XDP_SESSION_GET_CLASS (session)->close (session);

  g_object_unref (session);
}

static gboolean
handle_close (XdpDbusImplSession *object,
              GDBusMethodInvocation *invocation)
{
  XdpSession *session = (XdpSession *)object;

  if (!session->closed)
    xdp_session_close (session);

  xdp_dbus_impl_session_complete_close (object, invocation);

  return TRUE;
}

static void
xdp_session_skeleton_iface_init (XdpDbusImplSessionIface *iface)
{
  iface->handle_close = handle_close;
}

static void
xdp_session_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  XdpSession *session = (XdpSession *)object;

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
xdp_session_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  XdpSession *session = (XdpSession *)object;

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
xdp_session_finalize (GObject *object)
{
  XdpSession *session = (XdpSession *)object;

  g_hash_table_remove (sessions, session->id);

  g_free (session->id);

  G_OBJECT_CLASS (xdp_session_parent_class)->finalize (object);
}

static void
xdp_session_constructed (GObject *object)
{
  XdpSession *session = (XdpSession *)object;

  g_hash_table_insert (sessions, g_strdup (session->id), session);

  G_OBJECT_CLASS (xdp_session_parent_class)->constructed (object);
}

static void
xdp_session_init (XdpSession *session)
{
}

static void
xdp_session_class_init (XdpSessionClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->constructed = xdp_session_constructed;
  gobject_class->finalize = xdp_session_finalize;
  gobject_class->set_property = xdp_session_set_property;
  gobject_class->get_property = xdp_session_get_property;

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
