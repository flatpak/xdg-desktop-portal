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
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

typedef struct _ProxyResolver ProxyResolver;
typedef struct _ProxyResolverClass ProxyResolverClass;

struct _ProxyResolver
{
  XdpDbusProxyResolverSkeleton parent_instance;

  GProxyResolver *resolver;
  XdpDbusImplProxyResolver *impl;
  uint32_t impl_version;
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

static void
proxy_configure_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer     data)
{
  g_autoptr(GDBusMethodInvocation) invocation = data;
  ProxyResolver *proxy_resolver =
    (ProxyResolver *)g_object_get_data (G_OBJECT (invocation),
                                        "proxy-resolver");
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_proxy_resolver_call_configure_proxy_finish (proxy_resolver->impl, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Failed to configure proxy settings: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  xdp_dbus_proxy_resolver_complete_configure_proxy (XDP_DBUS_PROXY_RESOLVER (proxy_resolver), invocation);
}

static XdpOptionKey proxy_resolver_configure_proxy_options[] = {
  { "activation_token", G_VARIANT_TYPE_STRING, NULL },
  { "autoconfig_url", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
proxy_resolver_handle_configure_proxy (XdpDbusProxyResolver *object,
                                       GDBusMethodInvocation  *invocation,
                                       const char             *arg_parent_window,
                                       GVariant               *arg_options)
{
  ProxyResolver *proxy_resolver = (ProxyResolver *)object;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!proxy_resolver->impl)
    return G_DBUS_METHOD_INVOCATION_UNHANDLED;

  if (!xdp_filter_options (arg_options, &options_builder,
                           proxy_resolver_configure_proxy_options,
                           G_N_ELEMENTS (proxy_resolver_configure_proxy_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (invocation),
                          "proxy-resolver",
                          g_object_ref (proxy_resolver),
                          g_object_unref);

  xdp_dbus_impl_proxy_resolver_call_configure_proxy (proxy_resolver->impl,
                                                     arg_parent_window,
                                                     g_variant_builder_end (&options_builder),
                                                     NULL,
                                                     proxy_configure_cb,
                                                     g_object_ref (invocation));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
proxy_resolver_handle_get_is_cacheable (XdpDbusProxyResolver *object)
{
  ProxyResolver *resolver = (ProxyResolver *)object;
  gboolean is_cacheable;

  g_object_get(resolver->resolver,
               "is-cacheable",
               &is_cacheable,
               NULL);

  return is_cacheable;
}

static void
on_is_cacheable_changed (GObject    *gobject,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
  ProxyResolver *proxy_resolver = (ProxyResolver *)user_data;
  gboolean is_cacheable = FALSE;

  g_object_get (proxy_resolver->resolver,
                "is-cacheable",
                &is_cacheable,
                NULL);
  xdp_dbus_proxy_resolver_set_is_cacheable (XDP_DBUS_PROXY_RESOLVER (proxy_resolver),
                                            is_cacheable);
}

static void
proxy_resolver_dispose (GObject *object)
{
  ProxyResolver *resolver = (ProxyResolver *)object;

  g_clear_object (&resolver->resolver);
  g_clear_object (&resolver->impl);

  G_OBJECT_CLASS (proxy_resolver_parent_class)->dispose (object);
}

static void
proxy_resolver_iface_init (XdpDbusProxyResolverIface *iface)
{
  iface->handle_lookup = proxy_resolver_handle_lookup;
  iface->handle_lookup_multiple = proxy_resolver_handle_lookup_multiple;
  iface->handle_configure_proxy = proxy_resolver_handle_configure_proxy;
  iface->get_is_cacheable = proxy_resolver_handle_get_is_cacheable;
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
proxy_resolver_new (XdpContext                 *context,
                    XdpDbusImplProxyResolver   *impl)
{
  ProxyResolver *proxy_resolver;
  g_autoptr(GParamSpec) is_cacheable_spec = NULL;
  gboolean is_cacheable = FALSE;
  guint proxy_settings_changed_signal = 0;
  guint proxy_resolver_version = 1;

  proxy_resolver = g_object_new (proxy_resolver_get_type (), NULL);
  proxy_resolver->resolver = g_proxy_resolver_get_default ();

  if (impl)
    {
      proxy_resolver->impl = g_object_ref (impl);
    }

  is_cacheable_spec =
    g_object_class_find_property (G_OBJECT_GET_CLASS (proxy_resolver->resolver),
                                  "is-cacheable");
  proxy_settings_changed_signal =
    g_signal_lookup ("proxy-settings-changed",
                     G_OBJECT_TYPE (proxy_resolver->resolver));

  if (is_cacheable_spec && proxy_settings_changed_signal)
    {
      proxy_resolver_version = 2;
      g_object_get (proxy_resolver->resolver,
                    "is-cacheable",
                    &is_cacheable,
                    NULL);

      g_signal_connect (proxy_resolver->resolver, "notify::is-cacheable",
                        G_CALLBACK (on_is_cacheable_changed), proxy_resolver);
    }

  proxy_resolver->impl_version =
    proxy_resolver->impl
      ? MIN (xdp_dbus_impl_proxy_resolver_get_version (proxy_resolver->impl), 1) : 0;

  xdp_dbus_proxy_resolver_set_version (XDP_DBUS_PROXY_RESOLVER (proxy_resolver),
                                       proxy_resolver_version);

  xdp_dbus_proxy_resolver_set_is_cacheable (XDP_DBUS_PROXY_RESOLVER (proxy_resolver),
                                            is_cacheable);

  return proxy_resolver;
}

void
init_proxy_resolver (XdpContext *context)
{
  g_autoptr(ProxyResolver) proxy_resolver = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplProxyResolver) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, PROXY_RESOLVER_DBUS_IMPL_IFACE);
  if (impl_config)
    {
      impl = xdp_dbus_impl_proxy_resolver_proxy_new_sync (connection,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          impl_config->dbus_name,
                                                          DESKTOP_DBUS_PATH,
                                                          NULL, &error);
      /* this isn't a hard error, so don't return */
      if (impl == NULL)
        g_warning ("Failed to create proxy_resolver proxy: %s", error->message);
  }

  proxy_resolver = proxy_resolver_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&proxy_resolver)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);
}
