/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <glib-object.h>

typedef struct {
  char *source;
  char *dbus_name;
  char **interfaces;
  char **use_in;
  int priority;
} XdpImplConfig;

#define XDP_TYPE_PORTAL_CONFIG (xdp_portal_config_get_type())
G_DECLARE_FINAL_TYPE (XdpPortalConfig,
                      xdp_portal_config,
                      XDP, PORTAL_CONFIG,
                      GObject)

typedef struct _XdpContext XdpContext;

XdpPortalConfig * xdp_portal_config_new (XdpContext *config);

XdpImplConfig * xdp_portal_config_find (XdpPortalConfig *portal_config,
                                        const char      *impl_interface);

GPtrArray * xdp_portal_config_find_all (XdpPortalConfig *portal_config,
                                        const char      *impl_interface);
