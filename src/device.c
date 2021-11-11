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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>

#include "device.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#ifdef HAVE_PIPEWIRE
#include "pipewire.h"
#endif

#define PERMISSION_TABLE "devices"

typedef struct _Device Device;
typedef struct _DeviceClass DeviceClass;

struct _Device
{
  XdpDbusDeviceSkeleton parent_instance;
};

struct _DeviceClass
{
  XdpDbusDeviceSkeletonClass parent_class;
};

static XdpDbusImplAccess *impl;
static Device *device;
static XdpDbusImplLockdown *lockdown;

GType device_get_type (void) G_GNUC_CONST;
static void device_iface_init (XdpDbusDeviceIface *iface);

G_DEFINE_TYPE_WITH_CODE (Device, device, XDP_DBUS_TYPE_DEVICE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_DEVICE,
                                                device_iface_init));

static const char *known_devices[] = {
  "microphone",
  "speakers",
  "camera",
  NULL
};

Permission
device_get_permission_sync (const char *app_id,
                            const char *device)
{
  return get_permission_sync (app_id, PERMISSION_TABLE, device);
}

gboolean
device_query_permission_sync (const char *app_id,
                              const char *device,
                              Request    *request)
{
  Permission permission;
  gboolean allowed;

  permission = device_get_permission_sync (app_id, device);
  if (permission == PERMISSION_ASK || permission == PERMISSION_UNSET)
    {
      GVariantBuilder opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GAppInfo) info = NULL;
      g_autoptr(XdpDbusImplRequest) impl_request = NULL;

      if (app_id[0] != 0)
        {
          g_autofree char *desktop_id;
          desktop_id = g_strconcat (app_id, ".desktop", NULL);
          info = (GAppInfo*)g_desktop_app_info_new (desktop_id);
        }

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

      if (strcmp (device, "microphone") == 0)
        {
          g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("audio-input-microphone-symbolic"));

          title = g_strdup (_("Turn On Microphone?"));
          body = g_strdup (_("Access to your microphone can be changed "
                             "at any time from the privacy settings."));

          if (info == NULL)
            subtitle = g_strdup (_("An application wants to use your microphone."));
          else
            subtitle = g_strdup_printf (_("%s wants to use your microphone."), g_app_info_get_display_name (info));
        }
      else if (strcmp (device, "speakers") == 0)
        {
          g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("audio-speakers-symbolic"));

          title = g_strdup (_("Turn On Speakers?"));
          body = g_strdup (_("Access to your speakers can be changed "
                             "at any time from the privacy settings."));

          if (info == NULL)
            subtitle = g_strdup (_("An application wants to play sound."));
          else
            subtitle = g_strdup_printf (_("%s wants to play sound."), g_app_info_get_display_name (info));
        }
      else if (strcmp (device, "camera") == 0)
        {
          g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("camera-web-symbolic"));

          title = g_strdup (_("Turn On Camera?"));
          body = g_strdup (_("Access to your camera can be changed "
                             "at any time from the privacy settings."));

          if (info == NULL)
            subtitle = g_strdup (_("An application wants to use your camera."));
          else
            subtitle = g_strdup_printf (_("%s wants to use your camera."), g_app_info_get_display_name (info));
        }

      impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                           G_DBUS_PROXY_FLAGS_NONE,
                                                           g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                           request->id,
                                                           NULL, &error);
      if (!impl_request)
        return FALSE;

      request_set_impl_request (request, impl_request);

      g_debug ("Calling backend for device access to: %s", device);

      if (!xdp_dbus_impl_access_call_access_dialog_sync (impl,
                                                         request->id,
                                                         app_id,
                                                         "",
                                                         title,
                                                         subtitle,
                                                         body,
                                                         g_variant_builder_end (&opt_builder),
                                                         &response,
                                                         &results,
                                                         NULL,
                                                         &error))
        {
          g_warning ("A backend call failed: %s", error->message);
        }

      allowed = response == 0;

      if (permission == PERMISSION_UNSET)
        set_permission_sync (app_id, PERMISSION_TABLE, device, allowed ? PERMISSION_YES : PERMISSION_NO);
    }
  else
    allowed = permission == PERMISSION_YES ? TRUE : FALSE;

  return allowed;
}

#ifdef HAVE_PIPEWIRE
static struct {
  const char *device;
  const char *media_role;
  gboolean (*is_disabled) (XdpDbusImplLockdown *lockdown);
} device_checks[] = {
  { "camera", "Camera", xdp_dbus_impl_lockdown_get_disable_camera },
  { "microphone", "Speakers", xdp_dbus_impl_lockdown_get_disable_microphone },
  { "speakers", "Microphone", xdp_dbus_impl_lockdown_get_disable_sound_output },
};

static PipeWireRemote *
open_pipewire_remote (const char *app_id,
                      GPtrArray *media_roles,
                      GError **error)
{
  g_autofree char *roles = NULL;
  PipeWireRemote *remote;
  struct pw_permission permission_items[3];
  struct pw_properties *pipewire_properties;

  roles = g_strjoinv (",", (GStrv)media_roles->pdata);
  pipewire_properties =
    pw_properties_new ("pipewire.access.portal.app_id", app_id,
                       "pipewire.access.portal.media_roles", roles,
                       NULL);
  remote = pipewire_remote_new_sync (pipewire_properties,
                                     NULL, NULL, NULL, NULL,
                                     error);
  if (!remote)
    return NULL;

  /*
   * Hide all existing and future nodes by default. PipeWire will use the
   * permission store to set up permissions.
   */
  permission_items[0] = PW_PERMISSION_INIT (PW_ID_CORE, PW_PERM_RWX);
  permission_items[1] = PW_PERMISSION_INIT (remote->node_factory_id, PW_PERM_R);
  permission_items[2] = PW_PERMISSION_INIT (PW_ID_ANY, 0);

  pw_client_update_permissions (pw_core_get_client (remote->core),
                                G_N_ELEMENTS (permission_items),
                                permission_items);

  pipewire_remote_roundtrip (remote);

  return remote;
}
#endif

static gboolean
handle_open_pipewire_remote (XdpDbusDevice *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *fd_list,
                             GVariant *arg_options)
{
#ifdef HAVE_PIPEWIRE
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GPtrArray) media_roles = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) requested_devices = NULL;
  PipeWireRemote *remote;
  const char *app_id;
  size_t i;
  int fd_id;
  int fd;

  if (!g_variant_lookup (arg_options, "devices", "as", &requested_devices))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "No devices requested");
      return TRUE;
    }

  if (requested_devices == NULL || requested_devices[0] == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "No devices requested");
      return TRUE;
    }

  /* Check if any of the requested devices is invalid */
  for (i = 0; requested_devices[i]; i++)
    {
      gboolean found = FALSE;
      gsize j = 0;
      for (j = 0; j < G_N_ELEMENTS (device_checks); j++)
        {
          if (g_strcmp0 (requested_devices[i], device_checks[j].device))
            {
              found = TRUE;
              break;
            }
        }

      if (!found)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 "Invalid device requested");
          return TRUE;
        }
    }

  app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, &error);
  app_id = xdp_app_info_get_id (app_info);

  media_roles = g_ptr_array_new ();
  for (i = 0; i < G_N_ELEMENTS (device_checks); i++)
    {
      Permission permission;

      if (!g_strv_contains ((const char* const*)requested_devices,
                            device_checks[i].device))
        continue;

      if (!device_checks[i].is_disabled (lockdown))
        continue;

      permission = device_get_permission_sync (app_id, device_checks[i].device);
      if (permission != PERMISSION_YES)
        continue;

      g_ptr_array_add (media_roles, (gpointer) device_checks[i].media_role);
    }
  g_ptr_array_add (media_roles, NULL);

  if (media_roles->len == 1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed to access any of the requested devices");
      return TRUE;
    }

  remote = open_pipewire_remote (app_id, media_roles, &error);
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
  fd = pw_core_steal_fd (remote->core);
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

  xdp_dbus_device_complete_open_pipewire_remote (object, invocation,
                                                 out_fd_list,
                                                 g_variant_new_handle (fd_id));
  return TRUE;

#else /* HAVE_PIPEWIRE */
  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                         "PipeWire disabled");
  return TRUE;
#endif /* HAVE_PIPEWIRE */
}

static void
handle_access_device_in_thread (GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *app_id;
  const char *device;
  gboolean allowed;

  REQUEST_AUTOLOCK (request);

  app_id = (const char *)g_object_get_data (G_OBJECT (request), "app-id");
  device = (const char *)g_object_get_data (G_OBJECT (request), "device");

  allowed = device_query_permission_sync (app_id, device, request);

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS
                                              : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
                                      g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_access_device (XdpDbusDevice *object,
                      GDBusMethodInvocation *invocation,
                      guint32 pid,
                      const char * const *devices,
                      GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;

  if (g_strv_length ((char **)devices) != 1 || !g_strv_contains (known_devices, devices[0]))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid devices requested");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (g_str_equal (devices[0], "microphone") &&
      xdp_dbus_impl_lockdown_get_disable_microphone (lockdown))
    {
      g_debug ("Microphone access disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Microphone access disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  if (g_str_equal (devices[0], "camera") &&
      xdp_dbus_impl_lockdown_get_disable_camera (lockdown))
    {
      g_debug ("Camera access disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Camera access disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  if (g_str_equal (devices[0], "speakers") &&
      xdp_dbus_impl_lockdown_get_disable_sound_output (lockdown))
    {
      g_debug ("Speaker access disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Speaker access disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);

  if (!xdp_app_info_is_host (request->app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "This call is not available inside the sandbox");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  app_info = xdp_get_app_info_from_pid (pid, &error);
  if (app_info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid pid requested");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (request), "app-id", g_strdup (xdp_app_info_get_id (app_info)), g_free);
  g_object_set_data_full (G_OBJECT (request), "device", g_strdup (devices[0]), g_free);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                       request->id,
                                                       NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_device_complete_access_device (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_access_device_in_thread);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
device_iface_init (XdpDbusDeviceIface *iface)
{
  iface->handle_access_device = handle_access_device;
  iface->handle_open_pipewire_remote = handle_open_pipewire_remote;
}

static void
device_init (Device *device)
{
  xdp_dbus_device_set_version (XDP_DBUS_DEVICE (device), 2);
}

static void
device_class_init (DeviceClass *klass)
{
}

GDBusInterfaceSkeleton *
device_create (GDBusConnection *connection,
               const char *dbus_name,
               gpointer lockdown_proxy)
{
  g_autoptr(GError) error = NULL;

  lockdown = lockdown_proxy;

  impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              dbus_name,
                                              DESKTOP_PORTAL_OBJECT_PATH,
                                              NULL,
                                              &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  device = g_object_new (device_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (device);
}
