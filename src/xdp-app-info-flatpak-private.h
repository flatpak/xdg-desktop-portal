/*
 * Copyright © 2024 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "xdp-app-info-private.h"

struct _XdpAppInfoFlatpakClass
{
  XdpAppInfoClass parent_class;
};

#define XDP_TYPE_APP_INFO_FLATPAK (xdp_app_info_flatpak_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoFlatpak,
                      xdp_app_info_flatpak,
                      XDP, APP_INFO_FLATPAK,
                      XdpAppInfo)

XdpAppInfo * xdp_app_info_flatpak_new (int      pid,
                                       int      pidfd,
                                       GError **error);
