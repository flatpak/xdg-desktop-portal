/*
 * Copyright © 2017 Red Hat, Inc
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
 */

#pragma once

#include "xdp-request.h"
#include "xdp-call.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _XdpSession
{
  XdpDbusSessionSkeleton parent;

  GMutex mutex;

  gboolean exported;
  gboolean closed;

  char *app_id;
  char *id;
  char *token;

  char *sender;
  GDBusConnection *connection;

  char *impl_dbus_name;
  GDBusConnection *impl_connection;
  XdpDbusImplSession *impl_session;
} XdpSession;

typedef struct _XdpSessionClass
{
  XdpDbusSessionSkeletonClass parent_class;

  void (*close) (XdpSession *session);
} XdpSessionClass;

GType xdp_session_get_type (void);

G_GNUC_UNUSED static inline XdpSession *
XDP_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, xdp_session_get_type (), XdpSession);
}

G_GNUC_UNUSED static inline gboolean
XDP_IS_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, xdp_session_get_type ());
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpSession, g_object_unref)

#define SESSION_AUTOLOCK(session) \
  G_MUTEX_AUTO_LOCK (&session->mutex, G_PASTE (request_auto_locker, __LINE__));

#define SESSION_AUTOLOCK_UNREF(session) \
  G_MUTEX_AUTO_LOCK (&session->mutex, G_PASTE (request_auto_locker, __LINE__)); \
  g_autoptr(XdpSession) G_PASTE (request_auto_unref, __LINE__) = session;

const char * lookup_session_token (GVariant *options);

XdpSession * xdp_session_from_request (const char *session_handle,
                                       XdpRequest *request);

XdpSession * xdp_session_from_call (const char *session_handle,
                                    XdpCall    *call);

XdpSession * xdp_session_lookup (const char *session_handle);

void xdp_session_register (XdpSession *session);

gboolean xdp_session_export (XdpSession  *session,
                             GError     **error);

void close_sessions_for_sender (const char *sender);

void xdp_session_close (XdpSession *session,
                        gboolean    notify_close);
