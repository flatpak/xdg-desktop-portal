/*
 * Copyright © 2016 Red Hat, Inc
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
 */

#include "config.h"

#include "permissions.h"

static XdpImplPermissionStore *permission_store = NULL;

void
init_permission_store (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  permission_store = xdp_impl_permission_store_proxy_new_sync (connection,
                                                               G_DBUS_PROXY_FLAGS_NONE,
                                                               "org.freedesktop.impl.portal.PermissionStore",
                                                               "/org/freedesktop/impl/portal/PermissionStore",
                                                               NULL, &error);
  if (permission_store == NULL)
    g_warning ("No permission store: %s", error->message);
}

XdpImplPermissionStore *
get_permission_store (void)
{
  return permission_store;
}
