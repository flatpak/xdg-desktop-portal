/* background-monitor.h
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
