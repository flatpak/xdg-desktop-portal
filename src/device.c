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

static Device *device;

GType device_get_type (void) G_GNUC_CONST;
static void device_iface_init (XdpDbusDeviceIface *iface);

G_DEFINE_TYPE_WITH_CODE (Device, device, XDP_DBUS_TYPE_DEVICE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_DEVICE,
                                                device_iface_init));

static gboolean
handle_access_device (XdpDbusDevice *object,
                      GDBusMethodInvocation *invocation,
                      guint32 pid,
                      const char * const *devices,
                      GVariant *arg_options)
{
  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                         "This interface is deprecated and will be removed in a future version. "
                                         "Please contact upstream, if you are using this interface.");
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
device_create (GDBusConnection *connection)
{
  device = g_object_new (device_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (device);
}
