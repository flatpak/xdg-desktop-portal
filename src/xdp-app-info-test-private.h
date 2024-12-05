/*
 * Copyright © 2024 Red Hat, Inc
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "xdp-app-info-private.h"

struct _XdpAppInfoTestClass
{
  XdpAppInfoClass parent_class;
};

#define XDP_TYPE_APP_INFO_TEST (xdp_app_info_test_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoTest,
                      xdp_app_info_test,
                      XDP, APP_INFO_TEST,
                      XdpAppInfo)

XdpAppInfo * xdp_app_info_test_new (const char *app_id,
                                    const char *usb_queries_str);
