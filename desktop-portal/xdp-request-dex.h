/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-dbus.h"
#include "xdp-types.h"

#define XDP_TYPE_REQUEST_DEX (xdp_request_dex_get_type ())
G_DECLARE_FINAL_TYPE (XdpRequestDex,
                      xdp_request_dex,
                      XDP, REQUEST_DEX,
                      XdpDbusRequestSkeleton)

DexFuture * xdp_request_dex_new (XdpContext             *context,
                                 XdpAppInfo             *app_info,
                                 GDBusInterfaceSkeleton *skeleton,
                                 GDBusProxy             *proxy_impl,
                                 GVariant               *arg_options);

void xdp_request_dex_emit_response (XdpRequestDex                *request,
                                    XdgDesktopPortalResponseEnum  response,
                                    GVariant                     *results);

const char * xdp_request_dex_get_object_path (XdpRequestDex *request);
