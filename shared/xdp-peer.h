/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <stdint.h>

#include <gio/gio.h>
#include <libdex.h>

typedef struct
{
  gatomicrefcount rc;
  uint32_t pid;
  int fd;
} XdpPeerCredentials;

#define XDP_TYPE_PEER_CREDENTIALS (xdp_peer_credentials_get_type ())
GType xdp_peer_credentials_get_type (void);

XdpPeerCredentials * xdp_peer_credentials_new (uint32_t pid,
                                               int      fd);

XdpPeerCredentials * xdp_peer_credentials_ref (XdpPeerCredentials *self);

void xdp_peer_credentials_unref (XdpPeerCredentials *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdpPeerCredentials, xdp_peer_credentials_unref);

#define XDP_TYPE_PEER (xdp_peer_get_type())
G_DECLARE_DERIVABLE_TYPE (XdpPeer,
                          xdp_peer,
                          XDP, PEER,
                          GObject);

const char * xdp_peer_get_key (XdpPeer *peer);

DexFuture * xdp_peer_resolve_credentials (XdpPeer *peer);
