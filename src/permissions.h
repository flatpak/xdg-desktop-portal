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

typedef enum _Permission
{
  PERMISSION_UNSET,
  PERMISSION_NO,
  PERMISSION_YES,
  PERMISSION_ASK
} Permission;

char **get_permissions_sync (const char *app_id,
                             const char *table,
                             const char *id);

void set_permissions_sync (const char *app_id,
                           const char *table,
                           const char *id,
                           const char * const *permissions);

Permission get_permission_sync (const char *app_id,
                                const char *table,
                                const char *id);

void set_permission_sync (const char *app_id,
                          const char *table,
                          const char *id,
                          Permission permission);

char **permissions_from_tristate (Permission permission);

Permission permissions_to_tristate (char **permissions);

gboolean init_permission_store (GDBusConnection  *connection,
                                GError          **error);
XdpDbusImplPermissionStore *get_permission_store (void);
