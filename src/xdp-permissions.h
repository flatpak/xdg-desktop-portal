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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <gio/gio.h>
#include "xdp-impl-dbus.h"

typedef enum _XdpPermission
{
  XDP_PERMISSION_UNSET,
  XDP_PERMISSION_NO,
  XDP_PERMISSION_YES,
  XDP_PERMISSION_ASK
} XdpPermission;

char **xdp_get_permissions_sync (const char *app_id,
                                 const char *table,
                                 const char *id);

void xdp_set_permissions_sync (const char         *app_id,
                               const char         *table,
                               const char         *id,
                               const char * const *permissions);

XdpPermission xdp_get_permission_sync (const char *app_id,
                                       const char *table,
                                       const char *id);

void xdp_set_permission_sync (const char    *app_id,
                              const char    *table,
                              const char    *id,
                              XdpPermission  permission);

char **xdp_permissions_from_tristate (XdpPermission permission);

XdpPermission xdp_permissions_to_tristate (char **permissions);

gboolean xdp_init_permission_store (GDBusConnection  *connection,
                                    GError          **err);

XdpDbusImplPermissionStore *xdp_get_permission_store (void);
