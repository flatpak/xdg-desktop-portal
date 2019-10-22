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

#include "request.h"

#include <string.h>

static void request_skeleton_iface_init (XdpImplRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (Request, request, XDP_IMPL_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_IMPL_TYPE_REQUEST, request_skeleton_iface_init))

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation)
{
  Request *request = (Request *)object;
  g_autoptr(GError) error = NULL;

  if (request->exported)
    request_unexport (request);

  xdp_impl_request_complete_close (XDP_IMPL_REQUEST (request), invocation);

  return TRUE;
}

static void
request_skeleton_iface_init (XdpImplRequestIface *iface)
{
  iface->handle_close = handle_close;
}

static void
request_init (Request *request)
{
}

static void
request_finalize (GObject *object)
{
  Request *request = (Request *)object;

  g_free (request->sender);
  g_free (request->app_id);
  g_free (request->id);

  G_OBJECT_CLASS (request_parent_class)->finalize (object);
}

static void
request_class_init (RequestClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize  = request_finalize;
}

Request *
request_new (const char *sender,
             const char *app_id,
             const char *id)
{
  Request *request;

  request = g_object_new (request_get_type (), NULL);
  request->sender = g_strdup (sender);
  request->app_id = g_strdup (app_id);
  request->id = g_strdup (id);

  return request;
}

void
request_export (Request *request,
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
request_unexport (Request *request)
{
  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}
