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

  XdpAppInfoRegistry *app_info_registry;
};

struct _RegistryClass
{
  XdpDbusHostRegistrySkeletonClass parent_class;
};

GType registry_get_type (void) G_GNUC_CONST;
static void registry_iface_init (XdpDbusHostRegistryIface *iface);

G_DEFINE_TYPE_WITH_CODE (Registry, registry,
                         XDP_DBUS_HOST_TYPE_REGISTRY_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_HOST_TYPE_REGISTRY,
                                                registry_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Registry, g_object_unref)

static gboolean
handle_register (XdpDbusHostRegistry   *object,
                 GDBusMethodInvocation *invocation,
                 const char            *arg_app_id,
                 GVariant              *arg_options)
{
  Registry *registry = (Registry *) object;
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(XdpAppInfo) new_app_info = NULL;
  g_autoptr(DexFuture) insert_future = NULL;
  g_autoptr(DexPromise) app_info_promise = NULL;
  gboolean success;
  g_autoptr(GError) error = NULL;

  /* First, let's add an insert operation, that way we block any further
   * app info activity on the connection, until the promise is resolved. */
  app_info_promise = dex_promise_new ();
  insert_future =
    xdp_app_info_registry_insert_future (registry->app_info_registry,
                                         invocation,
                                         DEX_FUTURE (dex_ref (app_info_promise)));

  /* Then we check if we actually should allow the caller to update the
   * app id */
  {
    g_autoptr(XdpAppInfo) detected_app_info = NULL;

    detected_app_info = xdp_app_info_new_for_invocation_sync (invocation,
                                                              NULL,
                                                              &error);
    if (!detected_app_info)
      {
        g_debug ("Failed to detect app info for %s: %s",
                 sender, error->message);
        dex_promise_reject (app_info_promise, g_steal_pointer (&error));
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               XDG_DESKTOP_PORTAL_ERROR,
                                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Can't manually register unknown application");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    if (!xdp_app_info_is_host (detected_app_info))
      {
        const char *engine = xdp_app_info_get_engine_display_name (detected_app_info);

        g_debug ("Non-host (%s) sender %s tried to register a new app id",
                 engine, sender);
        dex_promise_reject (app_info_promise,
                            g_error_new_literal (G_IO_ERROR,
                                                 G_IO_ERROR_PERMISSION_DENIED,
                                                 "Not a host app"));
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               XDG_DESKTOP_PORTAL_ERROR,
                                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Can't manually register a %s application",
                                               engine);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  new_app_info = xdp_app_info_new_for_registered_sync (invocation,
                                                       arg_app_id,
                                                       NULL,
                                                       &error);
  if (!new_app_info)
    {
      g_debug ("Can't create registered app for %s: %s",
               sender, error->message);
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Can't create registered app");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* And finally we can resolve the promise and wait for it to get applied */
  dex_promise_resolve_object (app_info_promise, g_object_ref (new_app_info));
  success = dex_await_boolean (g_steal_pointer (&insert_future), &error);

  if (!success && error)
    {
      g_debug ("Can't create registered app for %s: %s",
               sender, error->message);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Can't register app");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!success)
    {
      g_debug ("Connection %s already associated with an application ID", sender);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Connection already associated with an application ID");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_debug ("Added registered host app %s", xdp_app_info_get_id (new_app_info));

  xdp_dbus_host_registry_complete_register (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
registry_iface_init (XdpDbusHostRegistryIface *iface)
{
  iface->handle_register = handle_register;
}

static void
registry_dispose (GObject *object)
{
  Registry *registry = (Registry *) object;

  g_clear_object (&registry->app_info_registry);

  G_OBJECT_CLASS (registry_parent_class)->dispose (object);
}

static void
registry_init (Registry *registry)
{
}

static void
registry_class_init (RegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = registry_dispose;
}

static Registry *
registry_new (XdpAppInfoRegistry *app_info_registry)
{
  Registry *registry;

  registry = g_object_new (registry_get_type (), NULL);
  registry->app_info_registry = g_object_ref (app_info_registry);

  xdp_dbus_host_registry_set_version (XDP_DBUS_HOST_REGISTRY (registry), 1);

  return registry;
}

void
init_registry (XdpContext *context)
{
  g_autoptr(Registry) registry = NULL;
  XdpAppInfoRegistry *app_info_registry =
    xdp_context_get_app_info_registry (context);

  registry = registry_new (app_info_registry);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&registry)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER |
                                      XDP_CONTEXT_EXPORT_FLAGS_SKIP_AUTH);
}
