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

#include "xdp-utils.h"

#include "xdp-app-info.h"

typedef enum _XdpAppInfoFlags
{
  XDP_APP_INFO_FLAG_HAS_NETWORK = (1 << 0),
  XDP_APP_INFO_FLAG_SUPPORTS_OPATH = (1 << 1),
  XDP_APP_INFO_FLAG_REQUIRE_GAPPINFO = (1 << 2),
} XdpAppInfoFlags;

struct _XdpAppInfoClass
{
  GObjectClass parent_class;


  const char * (*get_permissions_id) (XdpAppInfo *app_info);

  gboolean (*is_valid_sub_app_id) (XdpAppInfo *app_info,
                                   const char *sub_app_id);

  char * (*remap_path) (XdpAppInfo *app_info,
                        const char *path);

  const GPtrArray * (*get_usb_queries) (XdpAppInfo *app_info);

  gboolean (*validate_autostart) (XdpAppInfo          *app_info,
                                  GKeyFile            *keyfile,
                                  const char * const  *autostart_exec,
                                  GCancellable        *cancellable,
                                  GError             **error);

  gboolean (*validate_dynamic_launcher) (XdpAppInfo  *app_info,
                                         GKeyFile    *key_file,
                                         GError     **error);

  GAppInfo * (*create_gappinfo) (XdpAppInfo *app_info);
};

