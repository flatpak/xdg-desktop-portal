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

#pragma once

#include <gio/gio.h>

#include "request.h"
#include "permissions.h"

Permission device_get_permission_sync (const char *app_id,
                                       const char *device);

gboolean device_query_permission_sync (const char  *app_id,
                                       const char  *device,
                                       Request     *request,
                                       const char **ids,
                                       const char **choices,
                                       char       **chosen);

GDBusInterfaceSkeleton * device_create (GDBusConnection *connection,
                                        const char      *dbus_name,
                                        gpointer         lockdown);
