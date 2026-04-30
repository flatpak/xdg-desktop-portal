/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
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
                      XdpAppInfo);

XdpAppInfo * xdp_app_info_host_new (const char *sender,
                                    int         pid,
                                    int        *pidfd);

XdpAppInfo *
xdp_app_info_host_new_registered (const char  *sender,
                                  int          pid,
                                  int          pidfd,
                                  const char  *app_id,
                                  GError     **error);

#if HAVE_LIBSYSTEMD
XDP_EXPORT_TEST
char * _xdp_app_info_host_parse_app_id_from_unit_name (const char *unit);
#endif
