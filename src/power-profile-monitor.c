/*
 * Copyright © 2021 Red Hat, Inc
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
 * Authors:
 *       Bastien Nocera <hadess@hadess.net>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-request.h"

#include "power-profile-monitor.h"

#if GLIB_CHECK_VERSION(2, 69, 1)
#define HAS_POWER_PROFILE_MONITOR 1
#endif

typedef struct _PowerProfileMonitor PowerProfileMonitor;
typedef struct _PowerProfileMonitorClass PowerProfileMonitorClass;

struct _PowerProfileMonitor
{
  XdpDbusPowerProfileMonitorSkeleton parent_instance;

#ifdef HAS_POWER_PROFILE_MONITOR
  GPowerProfileMonitor *monitor;
#endif /* HAS_POWER_PROFILE_MONITOR */
};

struct _PowerProfileMonitorClass
{
  XdpDbusPowerProfileMonitorSkeletonClass parent_class;
};

GType power_profile_monitor_get_type (void) G_GNUC_CONST;
static void power_profile_monitor_iface_init (XdpDbusPowerProfileMonitorIface *iface);

G_DEFINE_TYPE_WITH_CODE (PowerProfileMonitor, power_profile_monitor,
                         XDP_DBUS_TYPE_POWER_PROFILE_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_POWER_PROFILE_MONITOR,
                                                power_profile_monitor_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PowerProfileMonitor, g_object_unref)

static void
power_profile_monitor_iface_init (XdpDbusPowerProfileMonitorIface *iface)
{
}

#ifdef HAS_POWER_PROFILE_MONITOR
static void
on_power_saver_enabled_changed (GPowerProfileMonitor *monitor,
                                GParamSpec           *pspec,
                                PowerProfileMonitor  *ppm)
{
  xdp_dbus_power_profile_monitor_set_power_saver_enabled (XDP_DBUS_POWER_PROFILE_MONITOR (ppm),
                                                          g_power_profile_monitor_get_power_saver_enabled (ppm->monitor));
}
#endif /* HAS_POWER_PROFILE_MONITOR */

static void
power_profile_monitor_init (PowerProfileMonitor *ppm)
{
}

static void
power_profile_monitor_finalize (GObject *object)
{
#ifdef HAS_POWER_PROFILE_MONITOR
  PowerProfileMonitor *ppm = (PowerProfileMonitor *) object;

  g_clear_object (&ppm->monitor);
#endif /* HAS_POWER_PROFILE_MONITOR */

  G_OBJECT_CLASS (power_profile_monitor_parent_class)->finalize (object);
}

static void
power_profile_monitor_class_init (PowerProfileMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = power_profile_monitor_finalize;
}

static PowerProfileMonitor *
power_profile_monitor_new (void)
{
  PowerProfileMonitor *power_profile_monitor;

  power_profile_monitor = g_object_new (power_profile_monitor_get_type (), NULL);

#ifdef HAS_POWER_PROFILE_MONITOR
  power_profile_monitor->monitor = g_power_profile_monitor_dup_default ();
  g_signal_connect_object (power_profile_monitor->monitor,
                           "notify::power-saver-enabled",
                           G_CALLBACK (on_power_saver_enabled_changed),
                           power_profile_monitor,
                           G_CONNECT_DEFAULT);
#endif /* HAS_POWER_PROFILE_MONITOR */

  xdp_dbus_power_profile_monitor_set_version (
    XDP_DBUS_POWER_PROFILE_MONITOR (power_profile_monitor),
    1);

  return power_profile_monitor;
}

void
init_power_profile_monitor (XdpContext *context)
{
  g_autoptr(PowerProfileMonitor) power_profile_monitor = NULL;

  power_profile_monitor = power_profile_monitor_new ();

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&power_profile_monitor)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
