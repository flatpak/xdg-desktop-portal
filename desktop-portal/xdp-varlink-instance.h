/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <glib-object.h>

#include "xdp-app-info.h"

#define XDP_TYPE_VARLINK_INSTANCE (xdp_varlink_instance_get_type ())
G_DECLARE_FINAL_TYPE (XdpVarlinkInstance,
                      xdp_varlink_instance,
                      XDP, VARLINK_INSTANCE,
                      GObject);

XdpVarlinkInstance *xdp_varlink_instance_new (GHashTable *instances,
                                              const char *id,
                                              XdpAppInfo *app_info);

const char *xdp_varlink_instance_get_id (XdpVarlinkInstance *self);

XdpAppInfo *xdp_varlink_instance_get_app_info (XdpVarlinkInstance *self);
