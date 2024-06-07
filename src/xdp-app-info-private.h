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

#include "xdp-utils.h"

#include "xdp-app-info.h"

struct _XdpAppInfoClass
{
  GObjectClass parent_class;

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
};

void xdp_app_info_initialize (XdpAppInfo *app_info,
                              const char *engine,
                              const char *app_id,
                              const char *instance,
                              int         pidfd,
                              GAppInfo   *gappinfo,
                              gboolean    supports_opath,
                              gboolean    has_network,
                              gboolean    requires_pid_mapping);
