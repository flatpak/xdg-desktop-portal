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

#include "xdp-app-info.h"
#include "xdp-app-info-registry.h"
#include "xdp-context.h"
#include "xdp-host-dbus.h"
#include "xdp-utils.h"

typedef struct _Registry Registry;
typedef struct _RegistryClass RegistryClass;

struct _Registry
{
  XdpDbusHostRegistrySkeleton parent_instance;

  XdpContext *context;
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

static XdpAppInfo *
register_host_app_info_sync (GDBusMethodInvocation  *invocation,
                             const char             *app_id,
                             GCancellable           *cancellable,
                             GError                **error)
{
  XdpAppInfoRegistry *app_info_registry =
    xdp_context_get_app_info_registry (registry->context);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(XdpAppInfo) detected_app_info = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;

  if (xdp_app_info_registry_has_sender (app_info_registry, sender))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Connection already associated with an application ID");
      return NULL;
    }

  detected_app_info = xdp_app_info_new_for_invocation_sync (invocation,
                                                            cancellable,
                                                            error);
  if (!detected_app_info)
    return NULL;

  if (!xdp_app_info_is_host (detected_app_info))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't manually register a %s application",
                   xdp_app_info_get_engine_display_name (detected_app_info));
      return NULL;
    }

  app_info = xdp_app_info_new_for_registered_sync (invocation,
                                                   app_id,
                                                   cancellable,
                                                   error);
  if (!app_info)
    return NULL;

  g_debug ("Adding registered host app '%s'", xdp_app_info_get_id (app_info));

  xdp_app_info_registry_insert (app_info_registry, app_info);

  return g_steal_pointer (&app_info);
}

static gboolean
handle_register (XdpDbusHostRegistry   *object,
                 GDBusMethodInvocation *invocation,
                 const char            *arg_app_id,
                 GVariant              *arg_options)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  app_info = register_host_app_info_sync (invocation, arg_app_id, NULL, &error);
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

void
init_registry (XdpContext *context)
{
  registry = g_object_new (registry_get_type (), NULL);
  registry->context = context;

  xdp_context_export_host_portal (context, G_DBUS_INTERFACE_SKELETON (registry));

  g_object_set_data_full (G_OBJECT (context),
                          "-xdp-portal-registry",
                          registry,
                          g_object_unref);
}
