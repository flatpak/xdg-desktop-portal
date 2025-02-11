/*
 * Copyright © 2024 Red Hat, Inc
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
 *       Jonas Ådahl <jadahl@redhat.com>
 */

#include "config.h"

#include "registry.h"

#include <stdio.h>

#include "xdp-app-info-private.h"
#include "xdp-host-dbus.h"
#include "xdp-utils.h"

typedef struct _Registry Registry;
typedef struct _RegistryClass RegistryClass;

struct _Registry
{
  XdpDbusHostRegistrySkeleton parent_instance;
};

struct _RegistryClass
{
  XdpDbusHostRegistrySkeletonClass parent_class;
};

static Registry *registry;

GType registry_get_type (void) G_GNUC_CONST;
static void registry_iface_init (XdpDbusHostRegistryIface *iface);

G_DEFINE_TYPE_WITH_CODE (Registry, registry,
                         XDP_DBUS_HOST_TYPE_REGISTRY_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_HOST_TYPE_REGISTRY,
                                                registry_iface_init));

static gboolean
handle_register (XdpDbusHostRegistry   *object,
                 GDBusMethodInvocation *invocation,
                 const char            *arg_app_id,
                 GVariant              *arg_options)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  app_info = xdp_invocation_register_host_app_info_sync (invocation, arg_app_id,
                                                         NULL, &error);
  if (!app_info)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Could not register app ID: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (g_strcmp0 (xdp_app_info_get_id (app_info), arg_app_id) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Registered too late");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_host_registry_complete_register (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
registry_iface_init (XdpDbusHostRegistryIface *iface)
{
  iface->handle_register = handle_register;
}

static void
registry_init (Registry *registry)
{
  xdp_dbus_host_registry_set_version (XDP_DBUS_HOST_REGISTRY (registry), 1);
}

static void
registry_class_init (RegistryClass *klass)
{
}

GDBusInterfaceSkeleton *
registry_create (GDBusConnection *connection)
{
  registry = g_object_new (registry_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (registry);
}
