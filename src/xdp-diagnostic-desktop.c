#include "xdp-diagnostic-desktop.h"

#define XDP_IFACE_BASE_NAME "org.freedesktop.portal."
#define XDP_IMPL_DBUS_BASE_NAME "org.freedesktop.impl.portal.desktop."

#define DIAGNOSTIC_DESKTOP_BUS_NAME "org.freedesktop.diagnostic.portal.Desktop"
#define DIAGNOSTIC_DESKTOP_OBJECT_PATH "/org/freedesktop/diagnostic/portal/desktop"

struct PortalImplDetail {
  char *name;
  unsigned int version;
};

struct PortalDetail {
  unsigned int version;

  struct PortalImplDetail *impl;
  GPtrArray *impls;
};

struct _XdpDiagnosticDesktop
{
  XdpDbusDiagnosticDesktopSkeleton parent_instance;

  size_t xdp_iface_name_prefix_len;
  size_t xdp_impl_dbus_name_prefix_len;

  GDBusConnection *connection;

  struct PortalImplDetail *lockdown_impl;
  struct PortalImplDetail *access_impl;

  GHashTable *portals;
};

static XdpDiagnosticDesktop *_diagnostic_desktop = NULL;

static void g_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpDiagnosticDesktop,
                         xdp_diagnostic_desktop,
                         XDP_DBUS_DIAGNOSTIC_TYPE_DESKTOP_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, g_initable_iface_init))

static void
portal_impl_detail_free (struct PortalImplDetail *d)
{
  g_clear_pointer (&d->name, g_free);

  g_free (d);
}

static void
portal_detail_free (struct PortalDetail *d)
{
  g_clear_pointer (&d->impl, portal_impl_detail_free);
  g_clear_pointer (&d->impls, g_ptr_array_unref);

  g_free (d);
}

static GVariant *
portal_impl_detail_to_g_variant (struct PortalImplDetail *detail)
{
  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (detail->name));

  if (detail->version != 0)
    g_variant_builder_add (&builder, "{sv}", "version", g_variant_new_uint32 (detail->version));

  return g_variant_builder_end (&builder);
}

static struct PortalDetail *
xdp_diagnostic_desktop_lookup_portal_detail (XdpDiagnosticDesktop *self,
                                             const char           *name)
{
  struct PortalDetail *detail = g_hash_table_lookup (self->portals,
                                                     name);

  if (detail == NULL)
    {
      detail = g_new0 (struct PortalDetail, 1);
      g_hash_table_insert (self->portals, g_strdup (name), detail);
    }

  g_assert (detail != NULL);

  return detail;
}

static gboolean
request_freedesktop_diagnostic_desktop_name (XdpDiagnosticDesktop  *self,
                                             GCancellable          *cancellable,
                                             GError               **error)
{
  g_autoptr(GVariant) reply = NULL;
  GBusNameOwnerFlags flags;
  guint32 result;

  flags = G_BUS_NAME_OWNER_FLAGS_REPLACE;
#if GLIB_CHECK_VERSION(2,54,0)
  flags |= G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE;
#endif

  reply = g_dbus_connection_call_sync (self->connection,
                                       "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "RequestName",
                                       g_variant_new ("(su)", DIAGNOSTIC_DESKTOP_BUS_NAME, flags),
                                       G_VARIANT_TYPE ("(u)"),
                                       0, -1,
                                       cancellable,
                                       error);

  if (!reply)
    return FALSE;

  g_variant_get (reply, "(u)", &result);
  if (result != 1)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Failed to own diagnostic portal desktop D-Bus name");
      return FALSE;
    }

  return TRUE;
}

static gboolean
xdp_diagnostic_desktop_initable_init (GInitable     *initable,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  XdpDiagnosticDesktop *self = XDP_DIAGNOSTIC_DESKTOP (initable);
  g_autofree char *address = NULL;

  address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, cancellable, error);
  if (!address)
    return FALSE;

  self->connection = g_initable_new (G_TYPE_DBUS_CONNECTION,
                                     cancellable, error,
                                     "address", address,
                                     "flags", G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
#if GLIB_CHECK_VERSION(2,74,0)
                                              G_DBUS_CONNECTION_FLAGS_CROSS_NAMESPACE |
#endif
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                     "exit-on-close", TRUE,
                                     NULL);

  if (!self->connection)
    return FALSE;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self),
                                         self->connection,
                                         DIAGNOSTIC_DESKTOP_OBJECT_PATH,
                                         error))
    {
      return FALSE;
    }

  if (!request_freedesktop_diagnostic_desktop_name (self, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
g_initable_iface_init (GInitableIface *iface)
{
  iface->init = xdp_diagnostic_desktop_initable_init;
}

static void
xdp_diagnostic_desktop_finalize (GObject *object)
{
  XdpDiagnosticDesktop *self = XDP_DIAGNOSTIC_DESKTOP (object);

  g_clear_pointer (&self->lockdown_impl, portal_impl_detail_free);
  g_clear_pointer (&self->access_impl, portal_impl_detail_free);
  g_clear_pointer (&self->portals, g_hash_table_unref);

  if (self->connection)
    g_dbus_connection_flush_sync (self->connection, NULL, NULL);

  g_clear_object (&self->connection);

  G_OBJECT_CLASS (xdp_diagnostic_desktop_parent_class)->finalize (object);
}

static void
xdp_diagnostic_desktop_class_init (XdpDiagnosticDesktopClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_diagnostic_desktop_finalize;
}

static void
xdp_diagnostic_desktop_init (XdpDiagnosticDesktop *self)
{
  self->xdp_iface_name_prefix_len = strlen (XDP_IFACE_BASE_NAME);
  self->xdp_impl_dbus_name_prefix_len = strlen (XDP_IMPL_DBUS_BASE_NAME);
  self->portals = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         g_free,
                                         (GDestroyNotify)portal_detail_free);

  xdp_dbus_diagnostic_desktop_set_version (XDP_DBUS_DIAGNOSTIC_DESKTOP (self), 1);
}

XdpDiagnosticDesktop *
xdp_diagnostic_desktop_get (GError **error)
{
  if (_diagnostic_desktop == NULL)
    _diagnostic_desktop = g_initable_new (XDP_TYPE_DIAGNOSTIC_DESKTOP,
                                          NULL,
                                          error,
                                          NULL);

  return _diagnostic_desktop;
}

void
xdp_diagnostic_desktop_set_lockdown_impl (XdpDiagnosticDesktop *self,
                                          const char           *dbus_name)
{
  g_assert (XDP_IS_DIAGNOSTIC_DESKTOP (self));
  g_assert (self->lockdown_impl == NULL);
  g_assert (g_str_has_prefix (dbus_name, XDP_IMPL_DBUS_BASE_NAME));

  self->lockdown_impl = g_new0 (struct PortalImplDetail, 1);
  self->lockdown_impl->name = g_strdup (dbus_name + self->xdp_impl_dbus_name_prefix_len);
}

void
xdp_diagnostic_desktop_set_access_impl (XdpDiagnosticDesktop *self,
                                        const char           *dbus_name)
{
  g_assert (XDP_IS_DIAGNOSTIC_DESKTOP (self));
  g_assert (self->access_impl == NULL);
  g_assert (g_str_has_prefix (dbus_name, XDP_IMPL_DBUS_BASE_NAME));

  self->access_impl = g_new0 (struct PortalImplDetail, 1);
  self->access_impl->name = g_strdup (dbus_name + self->xdp_impl_dbus_name_prefix_len);
}

void
xdp_diagnostic_desktop_set_portal_unique_impl (XdpDiagnosticDesktop *self,
                                               const char           *portal_name,
                                               const char           *impl_dbus_name,
                                               unsigned int          version)
{
  struct PortalDetail *detail;
  struct PortalImplDetail *impl_detail;

  g_assert (XDP_IS_DIAGNOSTIC_DESKTOP (self));
  if (g_strcmp0 (portal_name, "Secret") == 0)
    g_assert (impl_dbus_name != NULL);
  else
    g_assert (g_str_has_prefix (impl_dbus_name, XDP_IMPL_DBUS_BASE_NAME));

  detail = xdp_diagnostic_desktop_lookup_portal_detail (self,
                                                        portal_name);

  g_assert (detail->impl == NULL);
  g_assert (detail->impls == NULL);

  impl_detail = g_new0 (struct PortalImplDetail, 1);

  if (g_str_has_prefix (impl_dbus_name, XDP_IMPL_DBUS_BASE_NAME))
    impl_detail->name = g_strdup (impl_dbus_name + self->xdp_impl_dbus_name_prefix_len);
  else
    impl_detail->name = g_strdup (impl_dbus_name);

  impl_detail->version = version;

  detail->impl = impl_detail;
}

void
xdp_diagnostic_desktop_add_portal_impl (XdpDiagnosticDesktop *self,
                                        const char           *portal_name,
                                        const char           *impl_dbus_name,
                                        unsigned int          version)
{
  struct PortalDetail *detail;
  struct PortalImplDetail *impl_detail;

  g_assert (XDP_IS_DIAGNOSTIC_DESKTOP (self));
  g_assert (g_str_has_prefix (impl_dbus_name, XDP_IMPL_DBUS_BASE_NAME));

  detail = xdp_diagnostic_desktop_lookup_portal_detail (self,
                                                        portal_name);

  g_assert (detail->impl == NULL);

  if (detail->impls == NULL)
      detail->impls = g_ptr_array_new_with_free_func ((GDestroyNotify)portal_impl_detail_free);

  impl_detail = g_new0 (struct PortalImplDetail, 1);
  impl_detail->name = g_strdup (impl_dbus_name + self->xdp_impl_dbus_name_prefix_len);
  impl_detail->version = version;

  g_ptr_array_add (detail->impls, impl_detail);
}

void
xdp_diagnostic_desktop_update_properties (XdpDiagnosticDesktop *self)
{
  g_autoptr (GVariant) portals;
  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sa{sv}}"));
  GHashTableIter iter;
  char *name;
  struct PortalDetail *detail;

  g_assert (XDP_IS_DIAGNOSTIC_DESKTOP (self));

  if (self->lockdown_impl != NULL)
    {
      xdp_dbus_diagnostic_desktop_set_lockdown_impl (XDP_DBUS_DIAGNOSTIC_DESKTOP (self),
                                                     portal_impl_detail_to_g_variant (self->lockdown_impl));
    }

  if (self->access_impl != NULL)
    {
      xdp_dbus_diagnostic_desktop_set_access_impl (XDP_DBUS_DIAGNOSTIC_DESKTOP (self),
                                                   portal_impl_detail_to_g_variant (self->access_impl));
    }

  g_hash_table_iter_init (&iter, self->portals);
  while (g_hash_table_iter_next (&iter, (gpointer) &name, (gpointer) &detail))
    {
      g_auto(GVariantBuilder) vardict = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      if (detail->impl != NULL)
        {
          g_variant_builder_add (&vardict,
                                 "{sv}",
                                 "implementation",
                                 portal_impl_detail_to_g_variant (detail->impl));
        }
      else if (detail->impls)
        {
          g_auto(GVariantBuilder) impls = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("av"));

          for (size_t i = 0; i < detail->impls->len; i++)
            {
              struct PortalImplDetail *impl_detail = g_ptr_array_index (detail->impls, i);

              g_variant_builder_add (&impls,
                                     "v",
                                     portal_impl_detail_to_g_variant (impl_detail));
            }

          g_variant_builder_add (&vardict, "{sv}", "implementations", g_variant_builder_end (&impls));
        }

      g_variant_builder_add (&builder, "{sa{sv}}", name, &vardict);
    }

  portals = g_variant_ref_sink (g_variant_builder_end (&builder));

  xdp_dbus_diagnostic_desktop_set_portals (XDP_DBUS_DIAGNOSTIC_DESKTOP (self),
                                           portals);
}
