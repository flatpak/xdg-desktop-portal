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

#include "xdp-types.h"

typedef enum _XdpContextExportFlags
{
  XDP_CONTEXT_EXPORT_FLAGS_NONE = 0,
  XDP_CONTEXT_EXPORT_FLAGS_HOST_PORTAL = (1 << 0),
} XdpContextExportFlags;

#define XDP_TYPE_CONTEXT (xdp_context_get_type())
G_DECLARE_FINAL_TYPE (XdpContext,
                      xdp_context,
                      XDP, CONTEXT,
                      GObject)

XdpContext * xdp_context_new (gboolean opt_verbose);

gboolean xdp_context_register (XdpContext       *context,
                               GDBusConnection  *connection,
                               GError          **error);

gboolean xdp_context_is_verbose (XdpContext *context);

XdpAppInfoRegistry * xdp_context_get_app_info_registry (XdpContext *context);

GDBusConnection * xdp_context_get_connection (XdpContext *context);

XdpPortalConfig * xdp_context_get_config (XdpContext *context);

XdpDbusImplLockdown * xdp_context_get_lockdown (XdpContext *context);

void xdp_context_take_and_export_portal (XdpContext             *context,
                                         GDBusInterfaceSkeleton *skeleton,
                                         XdpContextExportFlags   flags);

GDBusInterfaceSkeleton * xdp_context_get_portal (XdpContext *context,
                                                 const char *interface);
