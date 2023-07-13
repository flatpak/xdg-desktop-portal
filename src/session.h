/*
 * Copyright © 2017 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "request.h"
#include "call.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _Session Session;
typedef struct _SessionClass SessionClass;

struct _Session
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

  struct {
    gboolean has_transient_permissions;
  } persistence;
};

struct _SessionClass
{
  XdpDbusSessionSkeletonClass parent_class;

  void (*close) (Session *session);
};

GType session_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Session, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpDbusImplSession, g_object_unref)

const char * lookup_session_token (GVariant *options);

Session * acquire_session (const char *session_handle,
                           Request *request);

Session * acquire_session_from_call (const char *session_handle,
                                     Call *call);

Session * lookup_session (const char *session_handle);

void session_register (Session *session);

gboolean session_export (Session *session,
                         GError **error);

void close_sessions_for_sender (const char *sender);

void session_close (Session *session,
                    gboolean notify_close);

static inline void
auto_session_unlock_unref_helper (Session **session)
{
  if (!*session)
    return;

  g_mutex_unlock (&(*session)->mutex);
  g_object_unref (*session);
}

static inline Session *
auto_session_lock_helper (Session *session)
{
  if (session)
    g_mutex_lock (&session->mutex);
  return session;
}

#define SESSION_AUTOLOCK(session) \
  G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) \
  GMutex * G_PASTE (session_auto_unlock, __LINE__) = \
    auto_lock_helper (&session->mutex);

#define SESSION_AUTOLOCK_UNREF(session) \
  G_GNUC_UNUSED __attribute__((cleanup (auto_session_unlock_unref_helper))) \
  Session * G_PASTE (session_auto_unlock_unref, __LINE__) = \
    auto_session_lock_helper (session);
