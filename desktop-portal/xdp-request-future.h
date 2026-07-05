/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-dbus.h"
#include "xdp-types.h"

#define XDP_TYPE_REQUEST_FUTURE (xdp_request_future_get_type ())
G_DECLARE_FINAL_TYPE (XdpRequestFuture,
                      xdp_request_future,
                      XDP, REQUEST_FUTURE,
                      XdpDbusRequestSkeleton)

DexFuture * xdp_request_future_new (XdpContext             *context,
                                    XdpAppInfo             *app_info,
                                    GDBusInterfaceSkeleton *skeleton,
                                    GDBusProxy             *proxy_impl,
                                    GVariant               *arg_options);

void xdp_request_future_emit_response (XdpRequestFuture             *request,
                                       XdgDesktopPortalResponseEnum  response,
                                       GVariant                     *results);

const char * xdp_request_future_get_object_path (XdpRequestFuture *request);
