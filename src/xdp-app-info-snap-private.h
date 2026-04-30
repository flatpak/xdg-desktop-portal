/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-app-info-private.h"

struct _XdpAppInfoSnapClass
{
  XdpAppInfoClass parent_class;
};

#define XDP_TYPE_APP_INFO_SNAP (xdp_app_info_snap_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoSnap,
                      xdp_app_info_snap,
                      XDP, APP_INFO_SNAP,
                      XdpAppInfo);

XdpAppInfo * xdp_app_info_snap_new (const char  *sender,
                                    int          pid,
                                    int         *pidfd,
                                    GError     **error);

XDP_EXPORT_TEST
int _xdp_app_info_snap_parse_cgroup_file (FILE     *f,
                                          gboolean *is_snap);
