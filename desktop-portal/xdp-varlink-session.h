/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <stdint.h>

#include <varlink.h>

#include "xdp-session-dex.h"
#include "xdp-varlink-connection.h"

int64_t xdp_varlink_session_next_handle (void);

void xdp_varlink_session_park (XdpSessionDex        *session,
                               XdpVarlinkConnection *connection,
                               VarlinkCall          *call);
