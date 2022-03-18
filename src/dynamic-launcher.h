/*
 * Copyright © 2022 Matthew Leeds
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
 *       Matthew Leeds <mwleeds@protonmail.com>
 */

#pragma once

#include "config.h"

#ifdef HAVE_GLIB_2_66

#include <gio/gio.h>

#define XDG_PORTAL_APPLICATIONS_DIR "xdg-desktop-portal" G_DIR_SEPARATOR_S "applications"
#define XDG_PORTAL_ICONS_DIR "xdg-desktop-portal" G_DIR_SEPARATOR_S "icons"

GDBusInterfaceSkeleton * dynamic_launcher_create (GDBusConnection *connection,
                                                  const char      *dbus_name);

#endif
