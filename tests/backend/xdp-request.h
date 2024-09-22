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

#include "src/xdp-impl-dbus.h"

typedef struct _XdpRequest
{
  XdpDbusImplRequestSkeleton parent_instance;

  gboolean exported;
  char *sender;
  char *app_id;
  char *id;
} XdpRequest;

typedef struct _XdpRequestClass
{
  XdpDbusImplRequestSkeletonClass parent_class;
} XdpRequestClass;

GType xdp_request_get_type (void) G_GNUC_CONST;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpRequest, g_object_unref)

XdpRequest *xdp_request_new (const char *sender,
                             const char *app_id,
                             const char *id);

void xdp_request_export (XdpRequest      *request,
                         GDBusConnection *connection);

void xdp_request_unexport (XdpRequest *request);
