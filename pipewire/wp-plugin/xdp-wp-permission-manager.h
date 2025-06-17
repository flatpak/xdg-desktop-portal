/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <wp/wp.h>

#define XDP_WP_TYPE_PERMISSION_MANAGER (xdp_wp_permission_manager_get_type())
G_DECLARE_FINAL_TYPE (XdpWpPermissionManager, xdp_wp_permission_manager, XDP_WP, PERMISSION_MANAGER, GObject);

XdpWpPermissionManager * xdp_wp_permission_manager_new (WpCore          *core,
                                                        GDBusConnection *connection);
