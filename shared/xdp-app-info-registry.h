/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <libdex.h>

#include "xdp-types.h"

#define XDP_TYPE_APP_INFO_REGISTRY (xdp_app_info_registry_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoRegistry,
                      xdp_app_info_registry,
                      XDP, APP_INFO_REGISTRY,
                      GObject);

XdpAppInfoRegistry * xdp_app_info_registry_new (void);

DexFuture * xdp_app_info_registry_ensure_future (XdpAppInfoRegistry    *registry,
                                                 GDBusMethodInvocation *invocation);

DexFuture * xdp_app_info_registry_insert_future (XdpAppInfoRegistry    *registry,
                                                 GDBusMethodInvocation *invocation,
                                                 DexFuture             *app_info_future);

DexFuture * xdp_app_info_registry_delete_future (XdpAppInfoRegistry *registry,
                                                 const char         *sender);

