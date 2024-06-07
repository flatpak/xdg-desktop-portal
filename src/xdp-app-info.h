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

#include <stdio.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "glib-backports.h"

typedef enum _XdpAppInfoError
{
  XDP_APP_INFO_ERROR_WRONG_APP_KIND,
} XdpAppInfoError;

#define XDP_APP_INFO_ERROR (xdp_app_info_error_quark ())
GQuark xdp_app_info_error_quark (void);

#define XDP_TYPE_APP_INFO (xdp_app_info_get_type())
G_DECLARE_DERIVABLE_TYPE (XdpAppInfo,
                          xdp_app_info,
                          XDP, APP_INFO,
                          GObject)

gboolean xdp_app_info_is_host (XdpAppInfo *app_info);

const char * xdp_app_info_get_id (XdpAppInfo *app_info);

char * xdp_app_info_get_instance (XdpAppInfo *app_info);

GAppInfo * xdp_app_info_load_app_info (XdpAppInfo *app_info);

gboolean xdp_app_info_has_network (XdpAppInfo *app_info);

char * xdp_app_info_get_path_for_fd (XdpAppInfo   *app_info,
                                     int           fd,
                                     int           require_st_mode,
                                     struct stat  *st_buf,
                                     gboolean     *writable_out,
                                     GError      **error);

char * xdp_app_info_remap_path (XdpAppInfo *app_info,
                                const char *path);

gboolean xdp_app_info_map_pids (XdpAppInfo  *app_info,
                                pid_t       *pids,
                                guint        n_pids,
                                GError     **error);

gboolean xdp_app_info_map_tids (XdpAppInfo  *app_info,
                                pid_t        owner_pid,
                                pid_t       *tids,
                                guint        n_tids,
                                GError     **error);

gboolean xdp_app_info_pidfds_to_pids (XdpAppInfo  *app_info,
                                      const int   *fds,
                                      pid_t       *pids,
                                      gint         count,
                                      GError     **error);

gboolean xdp_app_info_validate_autostart (XdpAppInfo          *app_info,
                                          GKeyFile            *keyfile,
                                          const char * const  *autostart_exec,
                                          GCancellable        *cancellable,
                                          GError             **error);

gboolean xdp_app_info_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                                 GKeyFile    *key_file,
                                                 GError     **error);

XdpAppInfo * xdp_invocation_lookup_app_info_sync (GDBusMethodInvocation  *invocation,
                                                  GCancellable           *cancellable,
                                                  GError                **error);
