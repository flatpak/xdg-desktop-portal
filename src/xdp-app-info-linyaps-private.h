/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
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
                      XdpAppInfo);

XdpAppInfo * xdp_app_info_linyaps_new (const char *sender,
                                       int         pid,
                                       int        *pidfd,
                                       GError    **error);
