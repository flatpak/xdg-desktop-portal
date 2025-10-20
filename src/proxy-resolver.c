/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

#include "proxy-resolver.h"

typedef struct _ProxyResolver ProxyResolver;
typedef struct _ProxyResolverClass ProxyResolverClass;

struct _ProxyResolver
{
  XdpDbusProxyResolverSkeleton parent_instance;

  GProxyResolver *resolver;
};

struct _ProxyResolverClass
{
  XdpDbusProxyResolverSkeletonClass parent_class;
};

static ProxyResolver *proxy_resolver;

GType proxy_resolver_get_type (void) G_GNUC_CONST;
static void proxy_resolver_iface_init (XdpDbusProxyResolverIface *iface);

G_DEFINE_TYPE_WITH_CODE (ProxyResolver, proxy_resolver,
                         XDP_DBUS_TYPE_PROXY_RESOLVER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_PROXY_RESOLVER,
                                                proxy_resolver_iface_init));

static gboolean
proxy_resolver_handle_lookup (XdpDbusProxyResolver *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_uri)
{
  ProxyResolver *resolver = (ProxyResolver *)object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);

  if (!xdp_app_info_has_network (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "This call is not available inside the sandbox");
    }
  else
    {
      g_auto (GStrv) proxies = NULL;
      g_autoptr (GError) error = NULL;

      proxies = g_proxy_resolver_lookup (resolver->resolver, arg_uri, NULL, &error);
      if (!proxies)
        g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
      else
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(^as)", proxies));
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
proxy_resolver_dispose (GObject *object)
{
  ProxyResolver *resolver = (ProxyResolver *)object;

  g_clear_object (&resolver->resolver);
}

static void
proxy_resolver_iface_init (XdpDbusProxyResolverIface *iface)
{
  iface->handle_lookup = proxy_resolver_handle_lookup;
}

static void
proxy_resolver_init (ProxyResolver *resolver)
{
  resolver->resolver = g_proxy_resolver_get_default ();

  xdp_dbus_proxy_resolver_set_version (XDP_DBUS_PROXY_RESOLVER (resolver), 1);
}

static void
proxy_resolver_class_init (ProxyResolverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = proxy_resolver_dispose;
}

void
init_proxy_resolver (XdpContext *context)
{
  proxy_resolver = g_object_new (proxy_resolver_get_type (), NULL);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (proxy_resolver),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
