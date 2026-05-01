/* SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define XDP_TYPE_APP_LAUNCH_CONTEXT (xdp_app_launch_context_get_type())
G_DECLARE_FINAL_TYPE (XdpAppLaunchContext,
                      xdp_app_launch_context,
                      XDP, APP_LAUNCH_CONTEXT,
                      GAppLaunchContext)

XdpAppLaunchContext * xdp_app_launch_context_new (void);

void xdp_app_launch_context_set_activation_token (XdpAppLaunchContext *self,
                                                  const char          *token);

G_END_DECLS
