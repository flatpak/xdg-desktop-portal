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

struct _XdpAppInfoHostClass
{
  XdpAppInfoClass parent_class;
};

#define XDP_TYPE_APP_INFO_HOST (xdp_app_info_host_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoHost,
                      xdp_app_info_host,
                      XDP, APP_INFO_HOST,
                      XdpAppInfo)

XdpAppInfo * xdp_app_info_host_new (int pid,
                                    int pidfd);

XdpAppInfo *
xdp_app_info_host_new_registered (int          pid,
                                  int          pidfd,
                                  const char  *app_id,
                                  GError     **error);

#ifdef HAVE_LIBSYSTEMD
XDP_EXPORT_TEST
char * _xdp_app_info_host_parse_app_id_from_unit_name (const char *unit);
#endif
