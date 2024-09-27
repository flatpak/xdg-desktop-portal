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

#include "src/xdp-impl-dbus.h"

typedef struct _Session Session;
typedef struct _SessionClass SessionClass;

struct _Session
{
  XdpDbusImplSessionSkeleton parent;

  gboolean exported;
  gboolean closed;
  char *id;
};

struct _SessionClass
{
  XdpDbusImplSessionSkeletonClass parent_class;

  void (*close) (Session *session);
};

GType session_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Session, g_object_unref)

Session *lookup_session (const char *id);

Session *session_new (const char *id);

void session_close (Session *session);

gboolean session_export (Session *session,
                         GDBusConnection *connection,
                         GError **error);

void session_unexport (Session *session);
