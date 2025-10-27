/*
 * Copyright © 2025 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       ComixHe <heyuming@deepin.org>
 */

#pragma once

#include "xdp-app-info-private.h"

struct _XdpAppInfoLinyapsClass
{
  XdpAppInfoClass parent_class;
};

#define XDP_TYPE_APP_INFO_LINYAPS (xdp_app_info_linyaps_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoLinyaps,
                      xdp_app_info_linyaps,
                      XDP, APP_INFO_LINYAPS,
                      XdpAppInfo)

XdpAppInfo * xdp_app_info_linyaps_new (const char *sender,
                                       int         pid,
                                       int        *pidfd,
                                       GError    **error);
