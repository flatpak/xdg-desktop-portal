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
#include "xdp-dbus.h"

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
  GError *error = NULL;
  g_auto (GStrv) proxies = NULL;

  proxies = g_proxy_resolver_lookup (resolver->resolver, arg_uri, NULL, &error);
  if (error)
    g_dbus_method_invocation_take_error (invocation, error);
  else
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(^as)", proxies));
  return TRUE;
}

enum {
  PROP_0,
  PROP_VERSION
};

static void
proxy_resolver_set_property (GObject *object,
                             guint prop_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
proxy_resolver_get_property (GObject *object,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec)
{
  switch (prop_id)
    {
    case PROP_VERSION:
      g_value_set_uint (value, 1);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
proxy_resolver_class_init (ProxyResolverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = proxy_resolver_set_property;
  object_class->get_property = proxy_resolver_get_property;

  xdp_proxy_resolver_override_properties (object_class, PROP_VERSION);
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
}

GDBusInterfaceSkeleton *
proxy_resolver_create (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  proxy_resolver = g_object_new (proxy_resolver_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (proxy_resolver);
}
