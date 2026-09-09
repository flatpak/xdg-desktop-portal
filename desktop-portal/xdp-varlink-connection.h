/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <glib-object.h>

#include "xdp-app-info-registry.h"
#include "xdp-peer.h"
#include "xdp-varlink-instance.h"

#define XDP_TYPE_VARLINK_CONNECTION (xdp_varlink_connection_get_type ())
G_DECLARE_FINAL_TYPE (XdpVarlinkConnection,
                      xdp_varlink_connection,
                      XDP, VARLINK_CONNECTION,
                      GObject);

gboolean xdp_varlink_socket_cookie (int      socket_fd,
                                    guint64 *cookie_out);

XdpVarlinkConnection * xdp_varlink_connection_new (XdpAppInfoRegistry *app_info_registry,
                                                   int                 socket_fd,
                                                   guint64             cookie);

XdpPeer *xdp_varlink_connection_get_peer (XdpVarlinkConnection *self);

XdpVarlinkInstance *
xdp_varlink_connection_get_instance (XdpVarlinkConnection *self);

XdpAppInfo *xdp_varlink_connection_get_app_info (XdpVarlinkConnection *self);

void xdp_varlink_connection_claim (XdpVarlinkConnection *self,
                                   XdpVarlinkInstance *instance);
