/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <glib-object.h>
#include <varlink.h>

#include "xdp-app-info-registry.h"
#include "xdp-varlink-connection.h"
#include "xdp-varlink-instance.h"

#define PORTAL_VARLINK_SOCKET "org.freedesktop.portal"

#define XDP_VARLINK_ERROR_NOT_REGISTERED "NotRegistered"

#define XDP_TYPE_VARLINK_SERVICE (xdp_varlink_service_get_type ())
G_DECLARE_FINAL_TYPE (XdpVarlinkService,
                      xdp_varlink_service,
                      XDP, VARLINK_SERVICE,
                      GObject);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VarlinkObject, varlink_object_unref);

typedef enum
{
  XDP_VARLINK_METHOD_FLAGS_NONE = 0,

  XDP_VARLINK_METHOD_FLAGS_PRE_CLAIM = 1 << 0,
} XdpVarlinkMethodFlags;

typedef long (*XdpVarlinkMethodFunc) (XdpVarlinkService *service,
                                      XdpVarlinkConnection *connection,
                                      VarlinkCall *call,
                                      VarlinkObject *parameters,
                                      uint64_t flags,
                                      gpointer user_data);

typedef struct
{
  const char *name;
  XdpVarlinkMethodFunc func;
  XdpVarlinkMethodFlags flags;
} XdpVarlinkMethod;

XdpVarlinkService *
xdp_varlink_service_new (const char *socket_name,
                         XdpAppInfoRegistry *app_info_registry,
                         GError **error);

XdpAppInfoRegistry *
xdp_varlink_service_get_app_info_registry (XdpVarlinkService *self);

gboolean xdp_varlink_service_add_interface (XdpVarlinkService *self,
                                            const char *description,
                                            const XdpVarlinkMethod *methods,
                                            size_t n_methods,
                                            gpointer user_data,
                                            GDestroyNotify user_data_destroy,
                                            GError **error);
