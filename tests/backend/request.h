/*
 * Copyright © 2016 Red Hat, Inc
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
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include "xdg-desktop-portal-dbus.h"

typedef struct _Request Request;
typedef struct _RequestClass RequestClass;

struct _Request
{
  XdpImplRequestSkeleton parent_instance;

  gboolean exported;
  char *sender;
  char *app_id;
  char *id;
};

struct _RequestClass
{
  XdpImplRequestSkeletonClass parent_class;
};

GType request_get_type (void) G_GNUC_CONST;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Request, g_object_unref)

Request *request_new (const char *sender,
                      const char *app_id,
                      const char *id);

void request_export (Request *request,
                     GDBusConnection *connection);
void request_unexport (Request *request);
