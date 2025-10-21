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

#define APP_CHOOSER_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".AppChooser"

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

#define INHIBIT_DBUS_IFACE DESKTOP_DBUS_IFACE ".Inhibit"
#define INHIBIT_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Inhibit"
#define INHIBIT_PERMISSION_TABLE "inhibit"
#define INHIBIT_PERMISSION_ID "inhibit"

#define INPUT_CAPTURE_DBUS_IFACE DESKTOP_DBUS_IFACE ".InputCapture"
#define INPUT_CAPTURE_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".InputCapture"

#define LOCATION_DBUS_IFACE DESKTOP_DBUS_IFACE ".Location"
#define LOCATION_PERMISSION_TABLE "location"
#define LOCATION_PERMISSION_ID "location"

#define NOTIFICATION_DBUS_IFACE DESKTOP_DBUS_IFACE ".Notification"
#define NOTIFICATION_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Notification"
#define NOTIFICATION_PERMISSION_TABLE "notifications"
#define NOTIFICATION_PERMISSION_ID "notification"

#define OPEN_URI_DBUS_IFACE DESKTOP_DBUS_IFACE ".OpenURI"
#define OPEN_URI_PERMISSION_TABLE "desktop-used-apps"

#define POWER_PROFILE_MONITOR_DBUS_IFACE DESKTOP_DBUS_IFACE ".PowerProfileMonitor"

#define PRINT_DBUS_IFACE DESKTOP_DBUS_IFACE ".Print"
#define PRINT_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Print"

#define PROXY_RESOLVER_DBUS_IFACE DESKTOP_DBUS_IFACE ".ProxyResolver"

#define REALTIME_DBUS_IFACE DESKTOP_DBUS_IFACE ".Realtime"
#define REALTIME_PERMISSION_TABLE "realtime"
#define REALTIME_PERMISSION_ID "realtime"

#define REMOTE_DESKTOP_DBUS_IFACE DESKTOP_DBUS_IFACE ".RemoteDesktop"
#define REMOTE_DESKTOP_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".RemoteDesktop"
#define REMOTE_DESKTOP_PERMISSION_TABLE "remote-desktop"

#define SCREEN_CAST_DBUS_IFACE DESKTOP_DBUS_IFACE ".ScreenCast"
#define SCREEN_CAST_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".ScreenCast"
#define SCREEN_CAST_PERMISSION_TABLE "screencast"

#define SCREENSHOT_DBUS_IFACE DESKTOP_DBUS_IFACE ".Screenshot"
#define SCREENSHOT_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Screenshot"
#define SCREENSHOT_PERMISSION_TABLE "screenshot"
#define SCREENSHOT_PERMISSION_ID "screenshot"

#define SECRET_DBUS_IFACE DESKTOP_DBUS_IFACE ".Secret"
#define SECRET_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Secret"
