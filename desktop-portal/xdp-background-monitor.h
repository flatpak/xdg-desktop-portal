/* SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <glib-object.h>

#include "xdp-background-dbus.h"

G_BEGIN_DECLS

#define XDP_TYPE_BACKGROUND_MONITOR (xdp_background_monitor_get_type())
G_DECLARE_FINAL_TYPE (XdpBackgroundMonitor,
                      xdp_background_monitor,
                      XDP, BACKGROUND_MONITOR,
                      XdpDbusBackgroundMonitorSkeleton)

XdpBackgroundMonitor *xdp_background_monitor_new (GCancellable  *cancellable,
                                                  GError       **error);

G_END_DECLS
