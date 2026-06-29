/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
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
                      GObject);

XdpContext * xdp_context_new (gboolean opt_verbose);

gboolean xdp_context_register (XdpContext       *context,
                               GDBusConnection  *connection,
                               GError          **error);

gboolean xdp_context_is_verbose (XdpContext *context);

XdpAppInfoRegistry * xdp_context_get_app_info_registry (XdpContext *context);

GDBusConnection * xdp_context_get_connection (XdpContext *context);

XdpPortalConfig * xdp_context_get_config (XdpContext *context);

XdpDbusImplLockdown * xdp_context_get_lockdown_impl (XdpContext *context);

XdpDbusImplAccess * xdp_context_get_access_impl (XdpContext *context);

void xdp_context_take_and_export_portal (XdpContext             *context,
                                         GDBusInterfaceSkeleton *skeleton,
                                         XdpEntitlement          entitlement,
                                         XdpContextExportFlags   flags);

GDBusInterfaceSkeleton * xdp_context_get_portal (XdpContext *context,
                                                 const char *interface);

gboolean xdp_context_claim_object_path (XdpContext *context,
                                        const char *object_path);

void xdp_context_unclaim_object_path (XdpContext *context,
                                      const char *object_path);
