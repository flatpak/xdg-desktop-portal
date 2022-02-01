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

#include <string.h>
#include <gio/gio.h>

#include "proxy-resolver.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

typedef struct _ProxyResolver ProxyResolver;
typedef struct _ProxyResolverClass ProxyResolverClass;

struct _ProxyResolver
{
  XdpProxyResolverSkeleton parent_instance;

  GProxyResolver *resolver;
};

struct _ProxyResolverClass
{
  XdpProxyResolverSkeletonClass parent_class;
};

static ProxyResolver *proxy_resolver;

GType proxy_resolver_get_type (void) G_GNUC_CONST;
static void proxy_resolver_iface_init (XdpProxyResolverIface *iface);

G_DEFINE_TYPE_WITH_CODE (ProxyResolver, proxy_resolver, XDP_TYPE_PROXY_RESOLVER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_PROXY_RESOLVER, proxy_resolver_iface_init));

static gboolean
proxy_resolver_handle_lookup (XdpProxyResolver *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_uri)
{
  ProxyResolver *resolver = (ProxyResolver *)object;
  Request *request = request_from_invocation (invocation);

  if (!xdp_app_info_has_network (request->app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "This call is not available inside the sandbox");
    }
  else
    {
      g_auto (GStrv) proxies = NULL;
      GError *error = NULL;

      proxies = g_proxy_resolver_lookup (resolver->resolver, arg_uri, NULL, &error);
      if (error)
        g_dbus_method_invocation_take_error (invocation, error);
      else
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(^as)", proxies));
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
proxy_resolver_iface_init (XdpProxyResolverIface *iface)
{
  iface->handle_lookup = proxy_resolver_handle_lookup;
}

static void
proxy_resolver_init (ProxyResolver *resolver)
{
  resolver->resolver = g_proxy_resolver_get_default ();

  xdp_proxy_resolver_set_version (XDP_PROXY_RESOLVER (resolver), 1);
}

static void
proxy_resolver_class_init (ProxyResolverClass *klass)
{
}

GDBusInterfaceSkeleton *
proxy_resolver_create (GDBusConnection *connection)
{
  proxy_resolver = g_object_new (proxy_resolver_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (proxy_resolver);
}
