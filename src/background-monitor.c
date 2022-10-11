/* background-monitor.c
 *
 * Copyright 2022 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "background-monitor.h"

#define BACKGROUND_MONITOR_BUS_NAME "org.freedesktop.background.Monitor"
#define BACKGROUND_MONITOR_OBJECT_PATH "/org/freedesktop/background/monitor"

struct _BackgroundMonitor
{
  XdpDbusBackgroundMonitorSkeleton parent_instance;

  GDBusConnection *connection;
};

static void g_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (BackgroundMonitor,
                         background_monitor,
                         XDP_DBUS_BACKGROUND_TYPE_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, g_initable_iface_init))

static gboolean
request_freedesktop_background_name (BackgroundMonitor  *self,
                                     GCancellable       *cancellable,
                                     GError            **error)
{
  g_autoptr(GVariant) reply = NULL;
  GBusNameOwnerFlags flags;
  guint32 result;

  flags = G_BUS_NAME_OWNER_FLAGS_REPLACE;
#if GLIB_CHECK_VERSION(2,54,0)
  flags |= G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE;
#endif

  reply = g_dbus_connection_call_sync (self->connection,
                                       "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "RequestName",
                                       g_variant_new ("(su)", BACKGROUND_MONITOR_BUS_NAME, flags),
                                       G_VARIANT_TYPE ("(u)"),
                                       0, -1,
                                       cancellable,
                                       error);

  if (!reply)
    return FALSE;

  g_variant_get (reply, "(u)", &result);
  if (result != 1)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Failed to own background monitor D-Bus name");
      return FALSE;
    }

  return TRUE;
}

static gboolean
background_monitor_initable_init (GInitable     *initable,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  BackgroundMonitor *self = BACKGROUND_MONITOR (initable);
  g_autofree char *address = NULL;

  address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, cancellable, error);
  if (!address)
    return FALSE;

  self->connection = g_initable_new (G_TYPE_DBUS_CONNECTION,
                                     cancellable, error,
                                     "address", address,
                                     "flags", G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
#if GLIB_CHECK_VERSION(2,74,0)
                                              G_DBUS_CONNECTION_FLAGS_CROSS_NAMESPACE |
#endif
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                     "exit-on-close", TRUE,
                                     NULL);

  if (!self->connection)
    return FALSE;

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (initable),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  /* TODO: dos it need to listen to 'g-authorize-method'? */

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (initable),
                                         self->connection,
                                         BACKGROUND_MONITOR_OBJECT_PATH,
                                         error))
    {
      return FALSE;
    }

  if (!request_freedesktop_background_name (self, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
g_initable_iface_init (GInitableIface *iface)
{
  iface->init = background_monitor_initable_init;
}

static void
background_monitor_finalize (GObject *object)
{
  BackgroundMonitor *self = (BackgroundMonitor *)object;

  if (self->connection)
    g_dbus_connection_flush_sync (self->connection, NULL, NULL);

  g_clear_object (&self->connection);

  G_OBJECT_CLASS (background_monitor_parent_class)->finalize (object);
}

static void
background_monitor_class_init (BackgroundMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = background_monitor_finalize;
}

static void
background_monitor_init (BackgroundMonitor *self)
{
  xdp_dbus_background_monitor_set_version (XDP_DBUS_BACKGROUND_MONITOR (self), 1);
}

BackgroundMonitor *
background_monitor_new (GCancellable  *cancellable,
                        GError       **error)
{
  return g_initable_new (BACKGROUND_TYPE_MONITOR,
                         cancellable,
                         error,
                         NULL);
}
