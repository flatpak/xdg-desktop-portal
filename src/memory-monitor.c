/*
 * Copyright © 2016, 2019 Red Hat, Inc
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
 *       Bastien Nocera <hadess@hadess.net>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "memory-monitor.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

#if GLIB_CHECK_VERSION(2, 63, 3)
#define HAS_MEMORY_MONITOR 1
#endif

typedef struct _MemoryMonitor MemoryMonitor;
typedef struct _MemoryMonitorClass MemoryMonitorClass;

struct _MemoryMonitor
{
  XdpMemoryMonitorSkeleton parent_instance;

#ifdef HAS_MEMORY_MONITOR
  GMemoryMonitor *monitor;
#endif /* HAS_MEMORY_MONITOR */
};

struct _MemoryMonitorClass
{
  XdpMemoryMonitorSkeletonClass parent_class;
};

static MemoryMonitor *memory_monitor;

GType memory_monitor_get_type (void) G_GNUC_CONST;
static void memory_monitor_iface_init (XdpMemoryMonitorIface *iface);

G_DEFINE_TYPE_WITH_CODE (MemoryMonitor, memory_monitor, XDP_TYPE_MEMORY_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_MEMORY_MONITOR, memory_monitor_iface_init));

static void
memory_monitor_iface_init (XdpMemoryMonitorIface *iface)
{
}

#ifdef HAS_MEMORY_MONITOR
static void
low_memory_warning_cb (GObject *object,
                       GMemoryMonitorWarningLevel level,
                       MemoryMonitor *mm)
{
  xdp_memory_monitor_emit_low_memory_warning (XDP_MEMORY_MONITOR (mm), level);
}
#endif /* HAS_MEMORY_MONITOR */

static void
memory_monitor_init (MemoryMonitor *mm)
{
#ifdef HAS_MEMORY_MONITOR
  mm->monitor = g_memory_monitor_dup_default ();
  g_signal_connect (mm->monitor, "low-memory-warning", G_CALLBACK (low_memory_warning_cb), mm);
#endif /* HAS_MEMORY_MONITOR */

  xdp_memory_monitor_set_version (XDP_MEMORY_MONITOR (mm), 1);
}

static void
memory_monitor_finalize (GObject *object)
{
#ifdef HAS_MEMORY_MONITOR
  MemoryMonitor *mm = (MemoryMonitor *) object;

  g_clear_object (&mm->monitor);
#endif /* HAS_MEMORY_MONITOR */

  G_OBJECT_CLASS (memory_monitor_parent_class)->finalize (object);
}

static void
memory_monitor_class_init (MemoryMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = memory_monitor_finalize;
}

GDBusInterfaceSkeleton *
memory_monitor_create (GDBusConnection *connection)
{
  memory_monitor = g_object_new (memory_monitor_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (memory_monitor);
}
