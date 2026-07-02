/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "memory-monitor.h"

#include <string.h>

#include <gio/gio.h>

#include "xdp-context.h"
#include "xdp-dbus.h"

#if GLIB_CHECK_VERSION(2, 63, 3)
#define HAS_MEMORY_MONITOR 1
#endif

typedef struct _MemoryMonitor MemoryMonitor;
typedef struct _MemoryMonitorClass MemoryMonitorClass;

struct _MemoryMonitor
{
  XdpDbusMemoryMonitorSkeleton parent_instance;

#ifdef HAS_MEMORY_MONITOR
  GMemoryMonitor *monitor;
#endif /* HAS_MEMORY_MONITOR */
};

struct _MemoryMonitorClass
{
  XdpDbusMemoryMonitorSkeletonClass parent_class;
};

GType memory_monitor_get_type (void);
static void memory_monitor_iface_init (XdpDbusMemoryMonitorIface *iface);

G_DEFINE_TYPE_WITH_CODE (MemoryMonitor, memory_monitor,
                         XDP_DBUS_TYPE_MEMORY_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_MEMORY_MONITOR, 
                                                memory_monitor_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MemoryMonitor, g_object_unref)

static void
memory_monitor_iface_init (XdpDbusMemoryMonitorIface *iface)
{
}

#ifdef HAS_MEMORY_MONITOR
static void
on_low_memory_warning (GMemoryMonitor             *object,
                       GMemoryMonitorWarningLevel  level,
                       MemoryMonitor              *mm)
{
  xdp_dbus_memory_monitor_emit_low_memory_warning (XDP_DBUS_MEMORY_MONITOR (mm),
                                                   level);
}
#endif /* HAS_MEMORY_MONITOR */

static void
memory_monitor_init (MemoryMonitor *mm)
{
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

static MemoryMonitor *
memory_monitor_new (void)
{
  MemoryMonitor *memory_monitor;

  memory_monitor = g_object_new (memory_monitor_get_type (), NULL);

#ifdef HAS_MEMORY_MONITOR
  memory_monitor->monitor = g_memory_monitor_dup_default ();
  g_signal_connect_object (memory_monitor->monitor, "low-memory-warning",
                           G_CALLBACK (on_low_memory_warning),
                           memory_monitor,
                           G_CONNECT_DEFAULT);
#endif /* HAS_MEMORY_MONITOR */

  xdp_dbus_memory_monitor_set_version (XDP_DBUS_MEMORY_MONITOR (memory_monitor), 1);

  return memory_monitor;
}

void
init_memory_monitor (XdpContext *context)
{
  g_autoptr(MemoryMonitor) memory_monitor = NULL;

  memory_monitor = memory_monitor_new ();

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&memory_monitor)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
