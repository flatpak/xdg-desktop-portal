/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <glib-object.h>

#include "xdp-types.h"

typedef struct {
  char *source;
  char *dbus_name;
  char **interfaces;
  char **use_in;
} XdpImplConfig;

#define XDP_TYPE_PORTAL_CONFIG (xdp_portal_config_get_type())
G_DECLARE_FINAL_TYPE (XdpPortalConfig,
                      xdp_portal_config,
                      XDP, PORTAL_CONFIG,
                      GObject);

XdpPortalConfig * xdp_portal_config_new (XdpContext *config);

XdpImplConfig * xdp_portal_config_find (XdpPortalConfig *portal_config,
                                        const char      *impl_interface);

GPtrArray * xdp_portal_config_find_all (XdpPortalConfig *portal_config,
                                        const char      *impl_interface);
