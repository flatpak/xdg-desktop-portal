/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <libdex.h>
#include <wp/wp.h>

DexFuture * xdp_wp_core_sync (WpCore *core);
DexFuture * xdp_wp_core_connect_sync (WpCore *core);
