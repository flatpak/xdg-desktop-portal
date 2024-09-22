/*
 * Copyright © 2016 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include "xdp-app-info.h"
#include "xdp-utils.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef enum {
  XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS = 0,
  XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
  XDG_DESKTOP_PORTAL_RESPONSE_OTHER
} XdgDesktopPortalResponseEnum;

typedef struct _XdpRequest
{
  XdpDbusRequestSkeleton parent_instance;

  gboolean exported;
  char *id;
  char *sender;
  GMutex mutex;
  XdpAppInfo *app_info;

  XdpDbusImplRequest *impl_request;
} XdpRequest;

typedef struct _XdpRequestClass
{
  XdpDbusRequestSkeletonClass parent_class;
} XdpRequestClass;

GType xdp_request_get_type (void) G_GNUC_CONST;

G_GNUC_UNUSED static inline XdpRequest *
XDP_REQUEST (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, xdp_request_get_type (), XdpRequest);
}

G_GNUC_UNUSED static inline gboolean
XDP_IS_REQUEST (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, xdp_request_get_type ());
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpRequest, g_object_unref)

void xdp_request_init_invocation (GDBusMethodInvocation *invocation,
                                  XdpAppInfo            *app_info);

XdpRequest *xdp_request_from_invocation (GDBusMethodInvocation *invocation);

void xdp_request_export (XdpRequest      *request,
                         GDBusConnection *connection);

void xdp_request_unexport (XdpRequest *request);

void close_requests_for_sender (const char *sender);

void xdp_request_set_impl_request (XdpRequest         *request,
                                   XdpDbusImplRequest *impl_request);

static inline void
auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *
auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

#define REQUEST_AUTOLOCK(request) G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) GMutex * G_PASTE (request_auto_unlock, __LINE__) = auto_lock_helper (&request->mutex);
