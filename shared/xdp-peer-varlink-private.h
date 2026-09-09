/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-peer-private.h"

struct _XdpPeerVarlinkClass
{
  XdpPeerClass parent_class;
};

#define XDP_TYPE_PEER_VARLINK (xdp_peer_varlink_get_type ())
G_DECLARE_FINAL_TYPE (XdpPeerVarlink,
                      xdp_peer_varlink,
                      XDP, PEER_VARLINK,
                      XdpPeer);

XdpPeer * xdp_peer_varlink_new (int         socket_fd,
                                const char *key);
