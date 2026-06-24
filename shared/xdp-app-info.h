/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include "xdp-types.h"

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
                          GObject);

XdpAppInfo * xdp_app_info_new_for_invocation_sync (GDBusMethodInvocation  *invocation,
                                                   GCancellable           *cancellable,
                                                   GError                **error);

XdpAppInfo * xdp_app_info_new_for_registered_sync (GDBusMethodInvocation  *invocation,
                                                   const char             *app_id,
                                                   GCancellable           *cancellable,
                                                   GError                **error);

gboolean xdp_app_info_is_host (XdpAppInfo *app_info);

const char * xdp_app_info_get_id (XdpAppInfo *app_info);

const char * xdp_app_info_get_instance (XdpAppInfo *app_info);

const char * xdp_app_info_get_engine (XdpAppInfo *app_info);

const char * xdp_app_info_get_sender (XdpAppInfo *app_info);

const char * xdp_app_info_get_app_display_name (XdpAppInfo *app_info);

const char * xdp_app_info_get_engine_display_name (XdpAppInfo *app_info);

GAppInfo * xdp_app_info_get_gappinfo (XdpAppInfo *app_info);

gboolean xdp_app_info_is_valid_sub_app_id (XdpAppInfo *app_info,
                                           const char *sub_app_id);

gboolean xdp_app_info_has_network (XdpAppInfo *app_info);

gboolean xdp_app_info_get_pidns (XdpAppInfo  *app_info,
                                 ino_t       *pidns_id_out,
                                 GError     **error);

char * xdp_app_info_get_path_for_fd (XdpAppInfo   *app_info,
                                     int           fd,
                                     int           require_st_mode,
                                     struct stat  *st_buf,
                                     gboolean     *writable_out,
                                     GError      **error);

gboolean xdp_app_info_validate_autostart (XdpAppInfo          *app_info,
                                          GKeyFile            *keyfile,
                                          const char * const  *autostart_exec,
                                          GCancellable        *cancellable,
                                          GError             **error);

gboolean xdp_app_info_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                                 GKeyFile    *key_file,
                                                 GError     **error);

const GPtrArray * xdp_app_info_get_usb_queries (XdpAppInfo *app_info);

XdpEntitlements * xdp_app_info_get_entitlements (XdpAppInfo *app_info);
