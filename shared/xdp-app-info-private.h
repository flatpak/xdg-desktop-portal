/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-app-info.h"
#include "xdp-utils.h"

typedef enum _XdpAppInfoFlags
{
  XDP_APP_INFO_FLAG_HAS_NETWORK = (1 << 0),
  XDP_APP_INFO_FLAG_SUPPORTS_OPATH = (1 << 1),
  XDP_APP_INFO_FLAG_REQUIRE_GAPPINFO = (1 << 2),
} XdpAppInfoFlags;

struct _XdpAppInfoClass
{
  GObjectClass parent_class;

  gboolean (*is_valid_sub_app_id) (XdpAppInfo *app_info,
                                   const char *sub_app_id);

  char * (*remap_path) (XdpAppInfo *app_info,
                        const char *path);

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

