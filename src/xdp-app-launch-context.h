/* launch-context.h
 *
 * Copyright 2024 Red Hat, Inc.
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
#include <gio/gio.h>

G_BEGIN_DECLS

#define XDP_TYPE_APP_LAUNCH_CONTEXT (xdp_app_launch_context_get_type())
G_DECLARE_FINAL_TYPE (XdpAppLaunchContext,
                      xdp_app_launch_context,
                      XDP, APP_LAUNCH_CONTEXT,
                      GAppLaunchContext)

XdpAppLaunchContext * xdp_app_launch_context_new (void);

void xdp_app_launch_context_set_activation_token (XdpAppLaunchContext *self,
                                                  const char          *token);

G_END_DECLS
