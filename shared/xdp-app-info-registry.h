/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include "xdp-types.h"

#define XDP_TYPE_APP_INFO_REGISTRY (xdp_app_info_registry_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoRegistry,
                      xdp_app_info_registry,
                      XDP, APP_INFO_REGISTRY,
                      GObject);

XdpAppInfoRegistry * xdp_app_info_registry_new (void);

gboolean xdp_app_info_registry_has_sender (XdpAppInfoRegistry *registry,
                                           const char         *sender);

void xdp_app_info_registry_insert (XdpAppInfoRegistry *registry,
                                   XdpAppInfo         *app_info);

void xdp_app_info_registry_delete (XdpAppInfoRegistry *registry,
                                   const char         *sender);

XdpAppInfo * xdp_app_info_registry_ensure_for_invocation_sync (XdpAppInfoRegistry     *registry,
                                                               GDBusMethodInvocation  *invocation,
                                                               GCancellable           *cancellable,
                                                               GError                **error);
