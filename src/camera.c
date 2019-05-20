/*
 * Copyright © 2018-2019 Red Hat, Inc
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

#include "config.h"

#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>
#include <stdio.h>

#include "device.h"
#include "request.h"
#include "permissions.h"
#include "pipewire.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _Camera Camera;
typedef struct _CameraClass CameraClass;

struct _Camera
{
  XdpCameraSkeleton parent_instance;
};

struct _CameraClass
{
  XdpCameraSkeletonClass parent_class;
};

static Camera *camera;

GType camera_get_type (void);
static void camera_iface_init (XdpCameraIface *iface);

G_DEFINE_TYPE_WITH_CODE (Camera, camera, XDP_TYPE_CAMERA_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_CAMERA,
                                                camera_iface_init))

static void
handle_access_camera_in_thread_func (GTask *task,
                                     gpointer source_object,
                                     gpointer task_data,
                                     GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *app_id;
  gboolean allowed;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  app_id = (const char *)g_object_get_data (G_OBJECT (request), "app-id");

  allowed = device_query_permission_sync (app_id, "camera", request->id);

  if (request->exported)
    {
      GVariantBuilder results;
      guint32 response;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);

      response = allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS
                         : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_access_camera (XdpCamera *object,
                      GDBusMethodInvocation *invocation,
                      GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id;
  g_autoptr(GTask) task = NULL;

  REQUEST_AUTOLOCK (request);

  app_id = xdp_app_info_get_id (request->app_info);

  g_object_set_data_full (G_OBJECT (request), "app-id", g_strdup (app_id), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_camera_complete_access_camera (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_access_camera_in_thread_func);

  return TRUE;
}

static PipeWireRemote *
open_pipewire_camera_remote (const char *app_id,
                             GError **error)
{
  PipeWireRemote *remote;
  struct spa_dict_item permission_items[1];
  struct pw_properties *pipewire_properties;

  pipewire_properties =
    pw_properties_new ("pipewire.access.portal.app_id", app_id,
                       "pipewire.access.portal.media_roles", "Camera",
                       NULL);
  remote = pipewire_remote_new_sync (pipewire_properties, error);
  if (!remote)
    return NULL;

  /*
   * Hide all existing and future nodes by default. PipeWire will use the
   * permission store to set up permissions.
   */
  permission_items[0].key = PW_CORE_PROXY_PERMISSIONS_DEFAULT;
  permission_items[0].value = "---";

  pw_core_proxy_permissions (pw_remote_get_core_proxy (remote->remote),
                             &SPA_DICT_INIT (permission_items,
                                             G_N_ELEMENTS (permission_items)));

  pipewire_remote_roundtrip (remote);

  return remote;
}

static gboolean
handle_open_pipewire_remote (XdpCamera *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *in_fd_list,
                             GVariant *arg_options)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  const char *app_id;
  Permission permission;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int fd;
  int fd_id;
  g_autoptr(GError) error = NULL;
  PipeWireRemote *remote;

  app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, &error);
  app_id = xdp_app_info_get_id (app_info);
  permission = device_get_permission_sync (app_id, "camera");
  if (permission != PERMISSION_YES)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Permission denied");
      return TRUE;
    }

  remote = open_pipewire_camera_remote (app_id, &error);
  if (!remote)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to open PipeWire remote: %s",
                                             error->message);
      return TRUE;
    }

  out_fd_list = g_unix_fd_list_new ();
  fd = pw_remote_steal_fd (remote->remote);
  fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);
  pipewire_remote_destroy (remote);

  if (fd_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to append fd: %s",
                                             error->message);
      return TRUE;
    }

  xdp_camera_complete_open_pipewire_remote (object, invocation,
                                            out_fd_list,
                                            g_variant_new_handle (fd_id));
  return TRUE;
}

static void
camera_iface_init (XdpCameraIface *iface)
{
  iface->handle_access_camera = handle_access_camera;
  iface->handle_open_pipewire_remote = handle_open_pipewire_remote;
}

static void
camera_init (Camera *camera)
{
  xdp_camera_set_version (XDP_CAMERA (camera), 1);
}

static void
camera_class_init (CameraClass *klass)
{
}

GDBusInterfaceSkeleton *
camera_create (GDBusConnection *connection)
{
  camera = g_object_new (camera_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (camera);
}
