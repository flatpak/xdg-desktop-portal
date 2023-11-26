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

          if (info)
            {
              title = g_strdup_printf (_("Allow %s to Use the Microphone?"), g_app_info_get_display_name (info));
              body = g_strdup_printf (_("%s wants to access recording devices."), g_app_info_get_display_name (info));
            }
          else
            {
              title = g_strdup (_("Allow app to Use the Microphone?"));
              body = g_strdup (_("An app wants to access recording devices."));
            }
        }
      else if (strcmp (device, "speakers") == 0)
        {
          g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("audio-speakers-symbolic"));

          if (info)
            {
              title = g_strdup_printf (_("Allow %s to Use the Speakers?"), g_app_info_get_display_name (info));
              body = g_strdup_printf (_("%s wants to access audio devices."), g_app_info_get_display_name (info));
            }
          else
            {
              title = g_strdup (_("Allow app to Use the Speakers?"));
              body = g_strdup (_("An app wants to access audio devices."));
            }
        }
      else if (strcmp (device, "camera") == 0)
        {
          g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("camera-web-symbolic"));

          if (info)
            {
              title = g_strdup_printf (_("Allow %s to Use the Camera?"), g_app_info_get_display_name (info));
              body = g_strdup_printf (_("%s wants to access camera devices."), g_app_info_get_display_name (info));
            }
          else
            {
              title = g_strdup (_("Allow app to Use the Camera?"));
              body = g_strdup (_("An app wants to access camera devices."));
            }
        }

      impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
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
                                                         "",
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
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
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
}

static void
device_init (Device *device)
{
  xdp_dbus_device_set_version (XDP_DBUS_DEVICE (device), 1);
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
