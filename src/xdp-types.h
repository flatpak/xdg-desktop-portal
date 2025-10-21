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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>

typedef struct _XdpContext XdpContext;
typedef struct _XdpAppInfo XdpAppInfo;
typedef struct _XdpAppInfoRegistry XdpAppInfoRegistry;
typedef struct _XdpPortalConfig XdpPortalConfig;
typedef struct _XdpDbusImplLockdown XdpDbusImplLockdown;
typedef struct _XdpDbusImplAccess XdpDbusImplAccess;

#define XDG_PORTAL_APPLICATIONS_DIR "xdg-desktop-portal" G_DIR_SEPARATOR_S "applications"
#define XDG_PORTAL_ICONS_DIR "xdg-desktop-portal" G_DIR_SEPARATOR_S "icons"

#define DBUS_DBUS_NAME "org.freedesktop.DBus"
#define DBUS_DBUS_IFACE "org.freedesktop.DBus"
#define DBUS_DBUS_PATH "/org/freedesktop/DBus"

#define DESKTOP_DBUS_NAME "org.freedesktop.portal.Desktop"
#define DESKTOP_DBUS_IFACE "org.freedesktop.portal"
#define DESKTOP_DBUS_IMPL_IFACE "org.freedesktop.impl.portal"
#define DESKTOP_DBUS_PATH "/org/freedesktop/portal/desktop"

#define ACCESS_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Access"

#define LOCKDOWN_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Lockdown"

#define ACCOUNT_DBUS_IFACE DESKTOP_DBUS_IFACE ".Account"
#define ACCOUNT_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Account"

#define BACKGROUND_DBUS_IFACE DESKTOP_DBUS_IFACE ".Background"
#define BACKGROUND_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Background"
#define BACKGROUND_PERMISSION_TABLE "background"
#define BACKGROUND_PERMISSION_ID "background"

#define CAMERA_DBUS_IFACE DESKTOP_DBUS_IFACE ".Camera"
#define CAMERA_PERMISSION_TABLE "devices"
#define CAMERA_PERMISSION_DEVICE_CAMERA "camera"

#define CLIPBOARD_DBUS_IFACE DESKTOP_DBUS_IFACE ".Clipboard"
#define CLIPBOARD_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Clipboard"

#define DYNAMIC_LAUNCHER_DBUS_IFACE DESKTOP_DBUS_IFACE ".DynamicLauncher"
#define DYNAMIC_LAUNCHER_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".DynamicLauncher"

#define EMAIL_DBUS_IFACE DESKTOP_DBUS_IFACE ".Email"
#define EMAIL_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Email"

#define FILE_CHOOSER_DBUS_IFACE DESKTOP_DBUS_IFACE ".FileChooser"
#define FILE_CHOOSER_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".FileChooser"

#define GAMEMODE_DBUS_IFACE DESKTOP_DBUS_IFACE ".GameMode"
#define GAMEMODE_PERMISSION_TABLE "gamemode"
#define GAMEMODE_PERMISSION_ID "gamemode"

#define GLOBAL_SHORTCUTS_DBUS_IFACE DESKTOP_DBUS_IFACE ".GlobalShortcuts"
#define GLOBAL_SHORTCUTS_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".GlobalShortcuts"
