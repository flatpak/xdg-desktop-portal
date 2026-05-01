/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-app-info.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _XdpRequest
{
  XdpDbusRequestSkeleton parent_instance;

  gboolean exported;
  char *id;
  char *sender;
  GMutex mutex;
  XdpAppInfo *app_info;
  XdpContext *context;

  XdpDbusImplRequest *impl_request;
} XdpRequest;

struct _XdpRequestClass
{
  XdpDbusRequestSkeletonClass parent_class;
};

#define XDP_TYPE_REQUEST (xdp_request_get_type ())
G_DECLARE_FINAL_TYPE (XdpRequest,
                      xdp_request,
                      XDP, REQUEST,
                      XdpDbusRequestSkeleton);

#define REQUEST_AUTOLOCK(request) \
  G_MUTEX_AUTO_LOCK (&request->mutex, G_PASTE (request_auto_locker, __LINE__));

gboolean xdp_request_init_invocation (GDBusMethodInvocation  *invocation,
                                      XdpContext             *context,
                                      XdpAppInfo             *app_info,
                                      GError                **error);

XdpRequest *xdp_request_from_invocation (GDBusMethodInvocation *invocation);

void xdp_request_export (XdpRequest      *request,
                         GDBusConnection *connection);

void xdp_request_unexport (XdpRequest *request);

const char *xdp_request_get_object_path (XdpRequest *request);

void xdp_request_set_impl_request (XdpRequest         *request,
                                   XdpDbusImplRequest *impl_request);
