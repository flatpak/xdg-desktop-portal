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

#include <gio/gio.h>
#include <string.h>

#include "proxy-resolver.h"
#include "xdp-app-info.h"
#include "xdp-call.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
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

static XdpDbusImplProxy *impl;
static ProxyResolver *proxy_resolver;
static guint32 proxy_resolver_version = 1;

GType proxy_resolver_get_type (void) G_GNUC_CONST;
static void proxy_resolver_iface_init (XdpDbusProxyResolverIface *iface);

G_DEFINE_TYPE_WITH_CODE (ProxyResolver, proxy_resolver, XDP_DBUS_TYPE_PROXY_RESOLVER_SKELETON, G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_PROXY_RESOLVER, proxy_resolver_iface_init));

static gboolean
proxy_resolver_handle_lookup (XdpDbusProxyResolver *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_uri)
{
  ProxyResolver *resolver = (ProxyResolver *) object;
  XdpCall *call = xdp_call_from_invocation (invocation);

  if (!xdp_app_info_has_network (call->app_info))
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
on_proxy_properties_changed (GObject *gobject,
                             GParamSpec *pspec,
                             ProxyResolver *resolver)
{
  const char *name = g_param_spec_get_name (pspec);
  XdpDbusImplProxy *p = XDP_DBUS_IMPL_PROXY (gobject);
  XdpDbusImplProxyIface *iface = XDP_DBUS_IMPL_PROXY_GET_IFACE (p);

  if (g_strcmp0 (name, "use_proxy") == 0)
    {
      xdp_dbus_proxy_resolver_set_use_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                             iface->get_use_proxy (p));
    }
  else if (g_strcmp0 (name, "pac_url") == 0)
    {
      xdp_dbus_proxy_resolver_set_pac_url (XDP_DBUS_PROXY_RESOLVER (resolver),
                                           iface->get_pac_url (p));
    }
  else if (g_strcmp0 (name, "https_proxy") == 0)
    {
      xdp_dbus_proxy_resolver_set_https_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                               iface->get_https_proxy (p));
    }
  else if (g_strcmp0 (name, "http_proxy") == 0)
    {
      xdp_dbus_proxy_resolver_set_http_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                              iface->get_http_proxy (p));
    }
  else if (g_strcmp0 (name, "socks_proxy") == 0)
    {
      xdp_dbus_proxy_resolver_set_socks_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                               iface->get_socks_proxy (p));
    }
  else if (g_strcmp0 (name, "ftp_proxy") == 0)
    {
      xdp_dbus_proxy_resolver_set_ftp_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                             iface->get_ftp_proxy (p));
    }
  else if (g_strcmp0 (name, "ignored_hosts") == 0)
    {
      xdp_dbus_proxy_resolver_set_ignored_hosts (XDP_DBUS_PROXY_RESOLVER (resolver),
                                                 iface->get_ignored_hosts (p));
    }
}

static void
proxy_resolver_dispose (GObject *object)
{
  ProxyResolver *resolver = (ProxyResolver *) object;

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

  xdp_dbus_proxy_resolver_set_version (XDP_DBUS_PROXY_RESOLVER (resolver), proxy_resolver_version);

  if (impl)
    {
      g_signal_connect (impl, "notify::use_proxy",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);
      xdp_dbus_proxy_resolver_set_use_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                             XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_use_proxy (impl));

      g_signal_connect (impl, "notify::pac_url",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);
      xdp_dbus_proxy_resolver_set_pac_url (XDP_DBUS_PROXY_RESOLVER (resolver),
                                           XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_pac_url (impl));

      g_signal_connect (impl, "notify::https_proxy",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);
      xdp_dbus_proxy_resolver_set_https_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                               XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_https_proxy (impl));

      g_signal_connect (impl, "notify::http_proxy",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);
      xdp_dbus_proxy_resolver_set_http_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                              XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_http_proxy (impl));

      g_signal_connect (impl, "notify::socks_proxy",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);
      xdp_dbus_proxy_resolver_set_socks_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                               XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_socks_proxy (impl));

      g_signal_connect (impl, "notify::ftp_proxy",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);

      xdp_dbus_proxy_resolver_set_ftp_proxy (XDP_DBUS_PROXY_RESOLVER (resolver),
                                             XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_ftp_proxy (impl));

      g_signal_connect (impl, "notify::ignored_hosts",
                        G_CALLBACK (on_proxy_properties_changed),
                        resolver);

      xdp_dbus_proxy_resolver_set_ignored_hosts (XDP_DBUS_PROXY_RESOLVER (resolver),
                                                 XDP_DBUS_IMPL_PROXY_GET_IFACE (impl)->get_ignored_hosts (impl));
    }
}

static void
proxy_resolver_class_init (ProxyResolverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = proxy_resolver_dispose;
}

GDBusInterfaceSkeleton *
proxy_resolver_create (GDBusConnection *connection,
                       const char *dbus_name)
{
  g_autoptr (GError) error = NULL;

  if (dbus_name)
    impl = xdp_dbus_impl_proxy_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               DESKTOP_PORTAL_OBJECT_PATH,
                                               NULL, &error);
  if (impl)
    proxy_resolver_version = 2;
  else if (error)
    g_warning ("Failed to create proxy portal backend: %s, falling back to proxy resolver 1.0", error->message);

  proxy_resolver = g_object_new (proxy_resolver_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (proxy_resolver);
}
