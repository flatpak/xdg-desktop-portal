/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-peer-private.h"

struct _XdpPeerDbusClass
{
  XdpPeerClass parent_class;
};

#define XDP_TYPE_PEER_DBUS (xdp_peer_dbus_get_type())
G_DECLARE_FINAL_TYPE (XdpPeerDbus,
                      xdp_peer_dbus,
                      XDP, PEER_DBUS,
                      XdpPeer);

XdpPeer * xdp_peer_dbus_new (GDBusConnection *connection,
                             const char      *sender);
