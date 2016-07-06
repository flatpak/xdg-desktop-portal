/*
 * Copyright © 2016 Red Hat, Inc
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
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "network-monitor.h"
#include "xdp-dbus.h"

typedef struct _NetworkMonitor NetworkMonitor;
typedef struct _NetworkMonitorClass NetworkMonitorClass;

struct _NetworkMonitor
{
  XdpNetworkMonitorSkeleton parent_instance;

  GNetworkMonitor *monitor;
};

struct _NetworkMonitorClass
{
  XdpNetworkMonitorSkeletonClass parent_class;
};

static NetworkMonitor *network_monitor;

GType network_monitor_get_type (void) G_GNUC_CONST;
static void network_monitor_iface_init (XdpNetworkMonitorIface *iface);

G_DEFINE_TYPE_WITH_CODE (NetworkMonitor, network_monitor, XDP_TYPE_NETWORK_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_NETWORK_MONITOR, network_monitor_iface_init));

static void
network_monitor_iface_init (XdpNetworkMonitorIface *iface)
{
}

static void
notify (GObject *object,
        GParamSpec *pspec,
        NetworkMonitor *nm)
{
  if (strcmp (pspec->name, "network-available") == 0)
    xdp_network_monitor_set_available (XDP_NETWORK_MONITOR (nm),
                                       g_network_monitor_get_network_available (nm->monitor));
  else if (strcmp (pspec->name, "network-metered") == 0)
    xdp_network_monitor_set_metered (XDP_NETWORK_MONITOR (nm),
                                     g_network_monitor_get_network_metered (nm->monitor));
  else if (strcmp (pspec->name, "connectivity") == 0)
    xdp_network_monitor_set_connectivity (XDP_NETWORK_MONITOR (nm),
                                          g_network_monitor_get_connectivity (nm->monitor));
}

static void
changed (GNetworkMonitor *monitor,
         gboolean available,
         XdpNetworkMonitor *nm)
{
  xdp_network_monitor_emit_changed (nm, available);
}

static void
network_monitor_init (NetworkMonitor *nm)
{
  nm->monitor = g_network_monitor_get_default ();

  g_signal_connect (nm->monitor, "notify", G_CALLBACK (notify), nm);
  g_signal_connect (nm->monitor, "network-changed", G_CALLBACK (changed), nm);

  xdp_network_monitor_set_available (XDP_NETWORK_MONITOR (nm),
                                     g_network_monitor_get_network_available (nm->monitor));
  xdp_network_monitor_set_metered (XDP_NETWORK_MONITOR (nm),
                                   g_network_monitor_get_network_metered (nm->monitor));
  xdp_network_monitor_set_connectivity (XDP_NETWORK_MONITOR (nm),
                                        g_network_monitor_get_connectivity (nm->monitor));
}

static void
network_monitor_class_init (NetworkMonitorClass *klass)
{
}

GDBusInterfaceSkeleton *
network_monitor_create (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  network_monitor = g_object_new (network_monitor_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (network_monitor);
}
