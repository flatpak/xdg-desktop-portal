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

#include "request.h"

#include <string.h>

static void xdp_request_skeleton_iface_init (XdpDbusImplRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpRequest, xdp_request, XDP_DBUS_IMPL_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_IMPL_TYPE_REQUEST,
                                                xdp_request_skeleton_iface_init))

static gboolean
handle_close (XdpDbusImplRequest *object,
              GDBusMethodInvocation *invocation)
{
  XdpRequest *request = (XdpRequest *)object;

  if (request->exported)
    xdp_request_unexport (request);

  xdp_dbus_impl_request_complete_close (XDP_DBUS_IMPL_REQUEST (request),
                                        invocation);

  return TRUE;
}

static void
xdp_request_skeleton_iface_init (XdpDbusImplRequestIface *iface)
{
  iface->handle_close = handle_close;
}

static void
xdp_request_init (XdpRequest *request)
{
}

static void
xdp_request_finalize (GObject *object)
{
  XdpRequest *request = (XdpRequest *)object;

  g_free (request->sender);
  g_free (request->app_id);
  g_free (request->id);

  G_OBJECT_CLASS (xdp_request_parent_class)->finalize (object);
}

static void
xdp_request_class_init (XdpRequestClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize  = xdp_request_finalize;
}

XdpRequest *
xdp_request_new (const char *sender,
                 const char *app_id,
                 const char *id)
{
  XdpRequest *request;

  request = g_object_new (xdp_request_get_type (), NULL);
  request->sender = g_strdup (sender);
  request->app_id = g_strdup (app_id);
  request->id = g_strdup (id);

  return request;
}

void
xdp_request_export (XdpRequest      *request,
                    GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         connection,
                                         request->id,
                                         &error))
    {
      g_warning ("error exporting request: %s\n", error->message);
      g_clear_error (&error);
    }

  g_object_ref (request);
  request->exported = TRUE;
}

void
xdp_request_unexport (XdpRequest *request)
{
  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}
