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

static gboolean
handle_get_available (XdpNetworkMonitor     *object,
                      GDBusMethodInvocation *invocation)
{
  NetworkMonitor *nm = (NetworkMonitor *)object;
  gboolean available = g_network_monitor_get_network_available (nm->monitor);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", available));

  return TRUE;
}

static gboolean
handle_get_metered (XdpNetworkMonitor     *object,
                    GDBusMethodInvocation *invocation)
{
  NetworkMonitor *nm = (NetworkMonitor *)object;
  gboolean metered = g_network_monitor_get_network_metered (nm->monitor);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", metered));

  return TRUE;
}

static gboolean
handle_get_connectivity (XdpNetworkMonitor     *object,
                         GDBusMethodInvocation *invocation)
{
  NetworkMonitor *nm = (NetworkMonitor *)object;
  guint connectivity = g_network_monitor_get_connectivity (nm->monitor); 

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", connectivity));

  return TRUE;
}

static void
network_monitor_iface_init (XdpNetworkMonitorIface *iface)
{
  iface->handle_get_available = handle_get_available;
  iface->handle_get_metered = handle_get_metered;
  iface->handle_get_connectivity = handle_get_connectivity;
}

static void
notify (GObject *object,
        GParamSpec *pspec,
        NetworkMonitor *nm)
{
  xdp_network_monitor_emit_changed (XDP_NETWORK_MONITOR (nm));
}

static void
network_monitor_init (NetworkMonitor *nm)
{
  nm->monitor = g_network_monitor_get_default ();

  g_signal_connect (nm->monitor, "notify", G_CALLBACK (notify), nm);

  xdp_network_monitor_set_version (XDP_NETWORK_MONITOR (nm), 2);
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
