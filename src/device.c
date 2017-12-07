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
#include <gio/gdesktopappinfo.h>

#include "device.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define TABLE_NAME "devices"

typedef struct _Device Device;
typedef struct _DeviceClass DeviceClass;

struct _Device
{
  XdpDeviceSkeleton parent_instance;
};

struct _DeviceClass
{
  XdpDeviceSkeletonClass parent_class;
};

static XdpImplAccess *impl;
static Device *device;

GType device_get_type (void) G_GNUC_CONST;
static void device_iface_init (XdpDeviceIface *iface);

G_DEFINE_TYPE_WITH_CODE (Device, device, XDP_TYPE_DEVICE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_DEVICE, device_iface_init));

typedef enum { UNSET, NO, YES, ASK } Permission;

static Permission
get_permission (const char *app_id,
                const char *device)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  const char **permissions;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   device,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
      return UNSET;
    }

  if (!g_variant_lookup (out_perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No permissions stored for: device %s, app %s", device, app_id);

      return UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
      return UNSET;
    }

  g_debug ("permission store: device %s, app %s -> %s", device, app_id, permissions[0]);

  if (strcmp (permissions[0], "yes") == 0)
    return YES;
  else if (strcmp (permissions[0], "no") == 0)
    return NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return UNSET;
}

static void
set_permission (const char *app_id,
                const char *device,
                Permission permission)
{
  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  if (permission == ASK)
    permissions[0] = "ask";
  else if (permission == YES)
    permissions[0] = "yes";
  else if (permission == NO)
    permissions[0] = "no";
  else
    {
      g_warning ("Wrong permission format, ignoring");
      return;
    }
  permissions[1] = NULL;

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           TABLE_NAME,
                                                           TRUE,
                                                           device,
                                                           app_id,
                                                           (const char * const*)permissions,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
    }
}

static const char *known_devices[] = {
  "microphone",
  "speakers",
  "camera",
  NULL
};

static void
handle_access_microphone_in_thread (GTask *task,
                                    gpointer source_object,
                                    gpointer task_data,
                                    GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *app_id;
  Permission permission;
  gboolean allowed;
  const char *device;

  REQUEST_AUTOLOCK (request);

  app_id = (const char *)g_object_get_data (G_OBJECT (request), "app-id");
  device = (const char *)g_object_get_data (G_OBJECT (request), "device");

  permission = get_permission (app_id, device);
  if (permission == ASK || permission == UNSET)
    {
      GVariantBuilder opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GAppInfo) info = NULL;

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

      g_debug ("Calling backend for device access to: %s", device);

      if (!xdp_impl_access_call_access_dialog_sync (impl,
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

      if (permission == UNSET)
        set_permission (app_id, device, allowed ? YES : NO);
    }
  else
    allowed = permission == YES ? TRUE : FALSE;

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_request_emit_response (XDP_REQUEST (request),
                                 allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_access_device (XdpDevice *object,
                      GDBusMethodInvocation *invocation,
                      guint32 pid,
                      const char * const *devices,
                      GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autofree char *app_id = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;

  REQUEST_AUTOLOCK (request);

  if (!g_str_equal (request->app_id, ""))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "This call is not available inside the sandbox");
      return TRUE;
    }

  if (g_strv_length ((char **)devices) != 1 || !g_strv_contains (known_devices, devices[0]))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid devices requested");
      return TRUE;
    }

  app_id = xdp_get_app_id_from_pid (pid, NULL);

  g_object_set_data_full (G_OBJECT (request), "app-id", g_strdup (app_id ? app_id : ""), g_free);
  g_object_set_data_full (G_OBJECT (request), "device", g_strdup (devices[0]), g_free);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_device_complete_access_device (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_access_microphone_in_thread);

  return TRUE;
}

static void
device_iface_init (XdpDeviceIface *iface)
{
  iface->handle_access_device = handle_access_device;
}

static void
device_init (Device *device)
{
  xdp_device_set_version (XDP_DEVICE (device), 1);
}

static void
device_class_init (DeviceClass *klass)
{
}

GDBusInterfaceSkeleton *
device_create (GDBusConnection *connection,
               const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_access_proxy_new_sync (connection,
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
