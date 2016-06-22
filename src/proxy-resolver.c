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

static void
proxy_resolver_class_init (ProxyResolverClass *klass)
{
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
