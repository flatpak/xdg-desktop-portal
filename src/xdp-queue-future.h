/*
 * Copyright © 2025 Red Hat, Inc
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
 */

#pragma once

#include <glib-object.h>
#include <libdex.h>

#define XDP_TYPE_QUEUE_FUTURE (xdp_queue_future_get_type ())
G_DECLARE_FINAL_TYPE (XdpQueueFuture,
                      xdp_queue_future,
                      XDP, QUEUE_FUTURE,
                      GObject)

#define XDP_TYPE_QUEUE_FUTURE_GUARD (xdp_queue_future_guard_get_type ())
G_DECLARE_FINAL_TYPE (XdpQueueFutureGuard,
                      xdp_queue_future_guard,
                      XDP, QUEUE_FUTURE_GUARD,
                      GObject)

XdpQueueFuture * xdp_queue_future_new (void);

DexFuture * xdp_queue_future_next (XdpQueueFuture *queue_future);
