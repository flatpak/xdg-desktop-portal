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

#include <glib-object.h>
#include <gio/gio.h>

#include "xdp-types.h"

#define XDP_TYPE_APP_INFO_REGISTRY (xdp_app_info_registry_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoRegistry,
                      xdp_app_info_registry,
                      XDP, APP_INFO_REGISTRY,
                      GObject)

typedef struct _XdpAppInfoRegistryLocker XdpAppInfoRegistryLocker;

void xdp_app_info_registry_locker_free (XdpAppInfoRegistryLocker *locker);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpAppInfoRegistryLocker,
                               xdp_app_info_registry_locker_free)

XdpAppInfoRegistry * xdp_app_info_registry_new (void);

XdpAppInfo * xdp_app_info_registry_lookup_sender (XdpAppInfoRegistry *registry,
                                                  const char         *sender);

XdpAppInfoRegistryLocker * xdp_app_info_registry_lock (XdpAppInfoRegistry *registry,
                                                       const char         *sender);

void xdp_app_info_registry_insert_unlocked (XdpAppInfoRegistry *registry,
                                            XdpAppInfo         *app_info);

XdpAppInfo * xdp_app_info_registry_lookup_unlocked (XdpAppInfoRegistry *registry,
                                                    const char         *sender);

void xdp_app_info_registry_delete (XdpAppInfoRegistry *registry,
                                   const char         *sender);

XdpAppInfo * xdp_app_info_registry_ensure_for_invocation_sync (XdpAppInfoRegistry     *registry,
                                                               GDBusMethodInvocation  *invocation,
                                                               GCancellable           *cancellable,
                                                               GError                **error);
