#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <wp/wp.h>

#define XDP_WP_TYPE_PERMISSION_MANAGER (xdp_wp_permission_manager_get_type())
G_DECLARE_FINAL_TYPE (XdpWpPermissionManager, xdp_wp_permission_manager, XDP_WP, PERMISSION_MANAGER, GObject)

XdpWpPermissionManager * xdp_wp_permission_manager_new (WpCore          *core,
                                                        GDBusConnection *connection);
