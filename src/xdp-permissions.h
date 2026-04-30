/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>
#include "xdp-app-info.h"
#include "xdp-impl-dbus.h"

typedef enum _XdpPermission
{
  XDP_PERMISSION_UNSET,
  XDP_PERMISSION_NO,
  XDP_PERMISSION_YES,
  XDP_PERMISSION_ASK
} XdpPermission;

char **xdp_get_permissions_sync (XdpAppInfo *app_info,
                                 const char *table,
                                 const char *id);

void xdp_set_permissions_sync (XdpAppInfo         *app_info,
                               const char         *table,
                               const char         *id,
                               const char * const *permissions);

XdpPermission xdp_get_permission_sync (XdpAppInfo *app_info,
                                       const char *table,
                                       const char *id);

void xdp_set_permission_sync (XdpAppInfo    *app_info,
                              const char    *table,
                              const char    *id,
                              XdpPermission  permission);

char **xdp_permissions_from_tristate (XdpPermission permission);

XdpPermission xdp_permissions_to_tristate (char **permissions);

gboolean xdp_init_permission_store (GDBusConnection  *connection,
                                    GError          **err);

XdpDbusImplPermissionStore *xdp_get_permission_store (void);
