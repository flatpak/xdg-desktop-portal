/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-app-info-private.h"

struct _XdpAppInfoFlatpakClass
{
  XdpAppInfoClass parent_class;
};

#define XDP_TYPE_APP_INFO_FLATPAK (xdp_app_info_flatpak_get_type())
G_DECLARE_FINAL_TYPE (XdpAppInfoFlatpak,
                      xdp_app_info_flatpak,
                      XDP, APP_INFO_FLATPAK,
                      XdpAppInfo);

XdpAppInfo * xdp_app_info_flatpak_new (const char  *sender,
                                       int          pid,
                                       int         *pidfd,
                                       GError     **error);
