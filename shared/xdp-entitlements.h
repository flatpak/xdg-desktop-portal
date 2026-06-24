/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stddef.h>

#include <glib-object.h>

#include "xdp-types.h"

#define XDP_TYPE_ENTITLEMENTS (xdp_entitlements_get_type ())
G_DECLARE_FINAL_TYPE (XdpEntitlements, xdp_entitlements, XDP, ENTITLEMENTS, GObject)

gboolean xdp_entitlements_is_granted (XdpEntitlements *self,
                                      XdpEntitlement   entitlement);

gboolean xdp_entitlements_check (XdpEntitlements  *self,
                                 GError          **error,
                                 ...) G_GNUC_NULL_TERMINATED;

XdpEntitlement xdp_entitlement_lookup (const char *entitlement);

const char * xdp_entitlement_get_name (XdpEntitlement entitlement);

unsigned int xdp_entitlement_get_version (XdpEntitlement entitlement);
