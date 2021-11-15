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

enum
{
  CAMERA,
  SPEAKERS,
  MICROPHONE,
  N_DEVICES,
};

struct _Device
{
  XdpDbusDeviceSkeleton parent_instance;

#ifdef HAVE_PIPEWIRE
  PipeWireRemote *pipewire_remote;
  GSource *pipewire_source;
  GFileMonitor *pipewire_socket_monitor;
  int64_t connect_timestamps[10];
  int connect_timestamps_i;
  GHashTable *devices[N_DEVICES];
#endif
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

#ifdef HAVE_PIPEWIRE
static gboolean create_pipewire_remote (Device *device,
                                        GError **error);
#endif

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

#ifdef HAVE_PIPEWIRE

static void
global_added_cb (PipeWireRemote *remote,
                 uint32_t id,
                 const char *type,
                 const struct spa_dict *props,
                 gpointer user_data)
{
  Device *device = user_data;
  const struct spa_dict_item *media_class;

  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;

  if (!props)
    return;

  media_class = spa_dict_lookup_item (props, PW_KEY_MEDIA_CLASS);
  if (!media_class)
    return;

  if (g_strcmp0 (media_class->value, "Video/Source") == 0)
    {
      const struct spa_dict_item *media_role;

      media_role = spa_dict_lookup_item (props, PW_KEY_MEDIA_ROLE);
      if (!media_role)
        return;

      if (g_strcmp0 (media_role->value, "Camera") != 0)
        return;

      g_hash_table_add (device->devices[CAMERA], GUINT_TO_POINTER (id));
      xdp_dbus_device_set_is_camera_present (XDP_DBUS_DEVICE (device),
                                             g_hash_table_size (device->devices[CAMERA]) > 0);
    }
  else if (g_strcmp0 (media_class->value, "Audio/Source") == 0)
    {
      g_hash_table_add (device->devices[MICROPHONE], GUINT_TO_POINTER (id));
      xdp_dbus_device_set_is_microphone_present (XDP_DBUS_DEVICE (device),
                                                 g_hash_table_size (device->devices[MICROPHONE]) > 0);
    }
  else if (g_strcmp0 (media_class->value, "Audio/Sink") == 0)
    {
      g_hash_table_add (device->devices[SPEAKERS], GUINT_TO_POINTER (id));
      xdp_dbus_device_set_is_speaker_present (XDP_DBUS_DEVICE (device),
                                              g_hash_table_size (device->devices[SPEAKERS]) > 0);
    }
}

static void
global_removed_cb (PipeWireRemote *remote,
                   uint32_t id,
                   gpointer user_data)
{
  Device *device = user_data;

  g_hash_table_remove (device->devices[CAMERA], GUINT_TO_POINTER (id));
  xdp_dbus_device_set_is_camera_present (XDP_DBUS_DEVICE (device),
                                    g_hash_table_size (device->devices[CAMERA]) > 0);

  g_hash_table_remove (device->devices[SPEAKERS], GUINT_TO_POINTER (id));
  xdp_dbus_device_set_is_speaker_present (XDP_DBUS_DEVICE (device),
                                          g_hash_table_size (device->devices[SPEAKERS]) > 0);

  g_hash_table_remove (device->devices[MICROPHONE], GUINT_TO_POINTER (id));
  xdp_dbus_device_set_is_microphone_present (XDP_DBUS_DEVICE (device),
                                             g_hash_table_size (device->devices[MICROPHONE]) > 0);
}

static void
pipewire_remote_error_cb (gpointer data,
                          gpointer user_data)
{
  Device *device = user_data;
  g_autoptr(GError) error = NULL;

  g_hash_table_remove_all (device->devices[CAMERA]);
  xdp_dbus_device_set_is_camera_present (XDP_DBUS_DEVICE (device), FALSE);

  g_hash_table_remove_all (device->devices[SPEAKERS]);
  xdp_dbus_device_set_is_speaker_present (XDP_DBUS_DEVICE (device), FALSE);

  g_hash_table_remove_all (device->devices[MICROPHONE]);
  xdp_dbus_device_set_is_microphone_present (XDP_DBUS_DEVICE (device), FALSE);

  g_clear_pointer (&device->pipewire_source, g_source_destroy);
  g_clear_pointer (&device->pipewire_remote, pipewire_remote_destroy);

  if (!create_pipewire_remote (device, &error))
    g_warning ("Failed connect to PipeWire: %s", error->message);
}

static gboolean
create_pipewire_remote (Device *device,
                        GError **error)
{
  struct pw_properties *pipewire_properties;
  const int n_connect_retries = G_N_ELEMENTS (device->connect_timestamps);
  int64_t now;
  int max_retries_ago_i;
  int64_t max_retries_ago;

  now = g_get_monotonic_time ();
  device->connect_timestamps[device->connect_timestamps_i] = now;

  max_retries_ago_i = (device->connect_timestamps_i + 1) % n_connect_retries;
  max_retries_ago = device->connect_timestamps[max_retries_ago_i];

  device->connect_timestamps_i =
    (device->connect_timestamps_i + 1) % n_connect_retries;

  if (max_retries_ago &&
      now - max_retries_ago < G_USEC_PER_SEC * 10)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tried to reconnect to PipeWire too often, giving up");
      return FALSE;
    }

  pipewire_properties = pw_properties_new ("pipewire.access.portal.is_portal", "true",
                                           "portal.monitor", "Camera,Speakers,Microphone",
                                           NULL);
  device->pipewire_remote = pipewire_remote_new_sync (pipewire_properties,
                                                      global_added_cb,
                                                      global_removed_cb,
                                                      pipewire_remote_error_cb,
                                                      device,
                                                      error);
  if (!device->pipewire_remote)
    return FALSE;

  device->pipewire_source =
    pipewire_remote_create_source (device->pipewire_remote);

  return TRUE;
}

static void
on_pipewire_socket_changed (GFileMonitor *monitor,
                            GFile *file,
                            GFile *other_file,
                            GFileMonitorEvent event_type,
                            Device *device)
{
  g_autoptr(GError) error = NULL;

  if (event_type != G_FILE_MONITOR_EVENT_CREATED)
    return;

  if (device->pipewire_remote)
    {
      g_debug ("PipeWire socket created after remote was created");
      return;
    }

  g_debug ("PipeWireSocket created, tracking devices");

  if (!create_pipewire_remote (device, &error))
    g_warning ("Failed connect to PipeWire: %s", error->message);
}

static gboolean
init_device_tracker (Device *device,
                     GError **error)
{
  g_autofree char *pipewire_socket_path = NULL;
  GFile *pipewire_socket;
  g_autoptr(GError) local_error = NULL;

  pipewire_socket_path = g_strdup_printf ("%s/pipewire-0",
                                          g_get_user_runtime_dir ());
  pipewire_socket = g_file_new_for_path (pipewire_socket_path);
  device->pipewire_socket_monitor =
    g_file_monitor_file (pipewire_socket, G_FILE_MONITOR_NONE, NULL, error);
  if (!device->pipewire_socket_monitor)
    return FALSE;

  g_signal_connect (device->pipewire_socket_monitor,
                    "changed",
                    G_CALLBACK (on_pipewire_socket_changed),
                    device);

  device->devices[CAMERA] = g_hash_table_new (NULL, NULL);
  device->devices[SPEAKERS] = g_hash_table_new (NULL, NULL);
  device->devices[MICROPHONE] = g_hash_table_new (NULL, NULL);

  if (!create_pipewire_remote (device, &local_error))
    g_warning ("Failed connect to PipeWire: %s", local_error->message);

  return TRUE;
}

#else

static gboolean
init_device_tracker (Device *device,
                     GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "PipeWire disabled");
  return FALSE;
}

#endif /* HAVE_PIPEWIRE */

static void
device_finalize (GObject *object)
{
#ifdef HAVE_PIPEWIRE
  Device *device = (Device *)object;

  g_clear_pointer (&device->pipewire_source, g_source_destroy);
  g_clear_pointer (&device->pipewire_remote, pipewire_remote_destroy);
  g_clear_pointer (&device->devices[CAMERA], g_hash_table_unref);
  g_clear_pointer (&device->devices[SPEAKERS], g_hash_table_unref);
  g_clear_pointer (&device->devices[MICROPHONE], g_hash_table_unref);
#endif

  G_OBJECT_CLASS (device_parent_class)->finalize (object);
}

static void
device_init (Device *device)
{
  g_autoptr(GError) error = NULL;

  xdp_dbus_device_set_version (XDP_DBUS_DEVICE (device), 2);

  if (!init_device_tracker (device, &error))
    g_warning ("Failed to track devices: %s", error->message);
}

static void
device_class_init (DeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = device_finalize;
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
