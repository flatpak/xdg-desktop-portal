/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "proxy-resolver.h"

#include <string.h>

#include <gio/gio.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

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

GType proxy_resolver_get_type (void);
static void proxy_resolver_iface_init (XdpDbusProxyResolverIface *iface);

G_DEFINE_TYPE_WITH_CODE (ProxyResolver, proxy_resolver,
                         XDP_DBUS_TYPE_PROXY_RESOLVER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_PROXY_RESOLVER,
                                                proxy_resolver_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ProxyResolver, g_object_unref)

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

static gboolean
proxy_resolver_handle_lookup_multiple (XdpDbusProxyResolver *object,
                                       GDBusMethodInvocation *invocation,
                                       const gchar *const *arg_uri)
{
  ProxyResolver *resolver = (ProxyResolver *)object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  GVariantBuilder builder;

  if (xdp_dbus_proxy_resolver_get_version (XDP_DBUS_PROXY_RESOLVER (resolver)) < 2)
    return G_DBUS_METHOD_INVOCATION_UNHANDLED;

  if (!xdp_app_info_has_network (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "This call is not available inside the sandbox");
    }
  else if (xdp_dbus_proxy_resolver_get_proxy_mode (XDP_DBUS_PROXY_RESOLVER (resolver)) == 1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "This call is not allowed when the proxy mode is not cacheable");
    }
  else
    {
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("aas"));

      for (gsize i = 0; arg_uri[i] != NULL; i++)
        {
          g_auto (GStrv) proxies = NULL;
          g_autoptr (GError) error = NULL;

          proxies = g_proxy_resolver_lookup (resolver->resolver,
                                             arg_uri[i],
                                             NULL,
                                             &error);
          if (!proxies)
            g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
          else
            g_variant_builder_add (&builder, "^as", proxies);
        }

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(aas)", &builder));
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static guint
proxy_resolver_handle_get_proxy_mode (XdpDbusProxyResolver *object)
{
  ProxyResolver *resolver = (ProxyResolver *)object;
  guint proxy_mode = 0;

  g_object_get (resolver->resolver,
                "proxy-mode",
                &proxy_mode,
                NULL);

  return proxy_mode;
}

static void
on_proxy_settings_changed (GObject    *gobject,
                           GParamSpec *pspec,
                           gpointer    user_data)
{
  ProxyResolver *proxy_resolver = (ProxyResolver *)user_data;

  xdp_dbus_proxy_resolver_emit_proxy_settings_changed (XDP_DBUS_PROXY_RESOLVER (proxy_resolver));
}

static void
on_proxy_mode_changed (GObject    *gobject,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  ProxyResolver *proxy_resolver = (ProxyResolver *)user_data;
  guint proxy_mode = 0;

  g_object_get (proxy_resolver->resolver,
                "proxy-mode",
                &proxy_mode,
                NULL);

  xdp_dbus_proxy_resolver_set_proxy_mode (XDP_DBUS_PROXY_RESOLVER (proxy_resolver),
                                          proxy_mode);
}

static void
proxy_resolver_dispose (GObject *object)
{
  ProxyResolver *resolver = (ProxyResolver *)object;

  g_clear_object (&resolver->resolver);

  G_OBJECT_CLASS (proxy_resolver_parent_class)->dispose (object);
}

static void
proxy_resolver_iface_init (XdpDbusProxyResolverIface *iface)
{
  iface->handle_lookup = proxy_resolver_handle_lookup;
  iface->handle_lookup_multiple = proxy_resolver_handle_lookup_multiple;
  iface->get_proxy_mode = proxy_resolver_handle_get_proxy_mode;
}

static void
proxy_resolver_init (ProxyResolver *resolver)
{
}

static void
proxy_resolver_class_init (ProxyResolverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = proxy_resolver_dispose;
}

static ProxyResolver *
proxy_resolver_new (void)
{
  ProxyResolver *proxy_resolver;
  g_autoptr(GParamSpec) proxy_mode_spec = NULL;
  guint proxy_settings_changed_signal = 0;
  guint proxy_mode = 0;
  guint proxy_resolver_version = 1;

  proxy_resolver = g_object_new (proxy_resolver_get_type (), NULL);
  proxy_resolver->resolver = g_proxy_resolver_get_default ();

  proxy_mode_spec =
    g_object_class_find_property (G_OBJECT_GET_CLASS (proxy_resolver->resolver),
                                  "proxy-mode");
  proxy_settings_changed_signal =
    g_signal_lookup ("proxy-settings-changed",
                     G_OBJECT_TYPE (proxy_resolver->resolver));

  if (proxy_mode_spec && proxy_settings_changed_signal)
    {
      proxy_resolver_version = 2;

      g_object_get (proxy_resolver->resolver,
                    "proxy-mode",
                    &proxy_mode,
                    NULL);

      g_signal_connect_object (proxy_resolver->resolver, "notify::proxy-mode",
                               G_CALLBACK (on_proxy_mode_changed), proxy_resolver,
                               G_CONNECT_DEFAULT);
      g_signal_connect_object (proxy_resolver->resolver, "proxy-settings-changed",
                               G_CALLBACK (on_proxy_settings_changed), proxy_resolver,
                               G_CONNECT_DEFAULT);
    }

  xdp_dbus_proxy_resolver_set_version (XDP_DBUS_PROXY_RESOLVER (proxy_resolver),
                                       proxy_resolver_version);
  xdp_dbus_proxy_resolver_set_proxy_mode (XDP_DBUS_PROXY_RESOLVER (proxy_resolver),
                                          proxy_mode);

  return proxy_resolver;
}

void
init_proxy_resolver (XdpContext *context)
{
  g_autoptr(ProxyResolver) proxy_resolver = NULL;

  proxy_resolver = proxy_resolver_new ();

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&proxy_resolver)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);
}
