/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-peer.h"

struct _XdpPeerClass
{
  GObjectClass parent_class;

  DexFuture * (*resolve_credentials) (XdpPeer *peer);
};
