/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "settings.h"

#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <glib/gi18n.h>

#include <libdex.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

struct _XdpSettings
{
  XdpDbusSettingsSkeleton parent_instance;

  GPtrArray *impls; /* XdpDbusImplSettings */
  GCancellable *cancellable; /* (owned) (not nullable) */
};

#define XDP_TYPE_SETTINGS (xdp_settings_get_type ())
G_DECLARE_FINAL_TYPE (XdpSettings,
                      xdp_settings,
                      XDP, SETTINGS,
                      XdpDbusSettingsSkeleton)

static void xdp_settings_iface_init (XdpDbusSettingsIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSettings,
                               xdp_settings,
                               XDP_DBUS_TYPE_SETTINGS_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SETTINGS,
                                                      xdp_settings_iface_init));

static void
merge_impl_settings (GHashTable *merged,
                     GVariant   *settings)
{
  GVariantIter iter;
  const char *namespace;
  GVariant *nsvalue;

  g_variant_iter_init (&iter, settings);
  while (g_variant_iter_next (&iter, "{&s@a{sv}}", &namespace, &nsvalue))
    {
      g_autoptr (GVariant) owned_nsvalue = NULL;
      g_autofree char *owned_namespace = NULL;
      g_autoptr (GVariantDict) dict = NULL;
      GVariantIter iter2;
      const char *key;
      GVariant *value;

      owned_nsvalue = nsvalue;

      if (!g_hash_table_steal_extended (merged, namespace,
                                        (gpointer *)&owned_namespace,
                                        (gpointer *)&dict))
        {
          dict = g_variant_dict_new (NULL);
          owned_namespace = g_strdup (namespace);
        }

      g_variant_iter_init (&iter2, nsvalue);
      while (g_variant_iter_loop (&iter2, "{sv}", &key, &value))
        g_variant_dict_insert_value (dict, key, value);

      g_hash_table_insert (merged,
                           g_steal_pointer (&owned_namespace),
                           g_steal_pointer (&dict));
    }
}

static GVariant *
merged_to_variant (GHashTable *merged)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(a{sa{sv}})"));
  const char *namespace;
  GVariantDict *dict;
  GHashTableIter iter;

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  g_hash_table_iter_init (&iter, merged);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *)&namespace,
                                 (gpointer *)&dict))
    {
      g_variant_builder_add (&builder, "{s@a{sv}}",
                             namespace,
                             g_variant_dict_end (dict));
    }

  g_variant_builder_close (&builder);

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
settings_handle_read_all (XdpDbusSettings       *object,
                          GDBusMethodInvocation *invocation,
                          const char    * const *arg_namespaces)
{
  XdpSettings *self = XDP_SETTINGS (object);
  g_autoptr(GHashTable) merged = NULL;
  g_autoptr(GVariant) settings = NULL;

  merged = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  g_free,
                                  (GDestroyNotify) g_variant_dict_unref);

  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadAllResult) result = NULL;
      g_autoptr(GError) error = NULL;
      size_t j = self->impls->len - i - 1;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_all_future (g_ptr_array_index (self->impls, j),
                                                     arg_namespaces),
        &error);

      if (result == NULL)
        g_warning ("Failed to ReadAll() from Settings implementation: %s",
                   error->message);
      else
        merge_impl_settings (merged, result->value);
    }

  settings = merged_to_variant (merged);
  g_dbus_method_invocation_return_value (invocation, settings);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
settings_handle_read (XdpDbusSettings       *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_namespace,
                      const char            *arg_key)
{
  XdpSettings *self = XDP_SETTINGS (object);

  g_debug ("Read %s %s", arg_namespace, arg_key);

  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadResult) result = NULL;
      g_autoptr(GError) error = NULL;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_future (g_ptr_array_index (self->impls, i),
                                                 arg_namespace,
                                                 arg_key),
        &error);

      if (result != NULL)
        {
          g_dbus_method_invocation_return_value (invocation,
                                                 g_variant_new ("(v)", result->value));
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_debug ("Failed to Read() from Settings implementation: %s",
               error->message);
    }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s",
           arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 _("Requested setting not found"));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
settings_handle_read_one (XdpDbusSettings       *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_namespace,
                          const char            *arg_key)
{
  XdpSettings *self = XDP_SETTINGS (object);

  g_debug ("ReadOne %s %s", arg_namespace, arg_key);

  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadResult) result = NULL;
      g_autoptr(GError) error = NULL;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_future (g_ptr_array_index (self->impls, i),
                                                 arg_namespace,
                                                 arg_key),
        &error);

      if (result != NULL)
        {
          g_dbus_method_invocation_return_value (invocation,
                                                 g_variant_new_tuple (&result->value, 1));
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_debug ("Failed to Read() from Settings implementation: %s",
               error->message);
    }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s",
           arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 _("Requested setting not found"));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static DexFuture *
settings_changed_fiber (gpointer      self_ptr,
                        unsigned int  impl_idx,
                        const char   *ns,
                        const char   *key,
                        GVariant     *value)
{
  /* self is not owned here, instead we cancel on dispose */
  XdpSettings *self = XDP_SETTINGS (self_ptr);

  /* Check if any higher priority impl provides this key; suppress if so */
  for (size_t i = 0; i < impl_idx; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadResult) result = NULL;
      g_autoptr(GError) error = NULL;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_future (g_ptr_array_index (self->impls, i),
                                                 ns, key),
        &error);

      if (result != NULL)
        return dex_future_new_for_boolean (FALSE);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return dex_future_new_for_boolean (FALSE);
    }

  g_debug ("Emitting changed for %s %s", ns, key);
  xdp_dbus_settings_emit_setting_changed (XDP_DBUS_SETTINGS (self),
                                          ns, key, value);

  return dex_future_new_for_boolean (TRUE);
}

static void
on_impl_settings_changed (XdpDbusImplSettings *impl,
                          const char          *arg_namespace,
                          const char          *arg_key,
                          GVariant            *arg_value,
                          XdpSettings         *self)
{
  unsigned int impl_idx;

  g_ptr_array_find (self->impls, impl, &impl_idx);

  if (impl_idx == 0)
    {
      g_debug ("Emitting changed for %s %s", arg_namespace, arg_key);
      xdp_dbus_settings_emit_setting_changed (XDP_DBUS_SETTINGS (self),
                                              arg_namespace, arg_key,
                                              arg_value);
      return;
    }

  dex_future_disown (
    dex_future_first (
      dex_scheduler_spawnv (NULL, 0,
                            G_CALLBACK (settings_changed_fiber),
                            5,
                            G_TYPE_POINTER, self,
                            G_TYPE_UINT, impl_idx,
                            G_TYPE_STRING, arg_namespace,
                            G_TYPE_STRING, arg_key,
                            G_TYPE_VARIANT, arg_value),
      dex_cancellable_new_from_cancellable (self->cancellable),
      NULL));
}

static void
xdp_settings_iface_init (XdpDbusSettingsIface *iface)
{
  iface->handle_read = settings_handle_read;
  iface->handle_read_one = settings_handle_read_one;
  iface->handle_read_all = settings_handle_read_all;
}

static void
xdp_settings_init (XdpSettings *self)
{
}

static void
xdp_settings_dispose (GObject *object)
{
  XdpSettings *self = XDP_SETTINGS (object);

  for (size_t i = 0; self->impls && i < self->impls->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->impls, i), self);

  g_clear_pointer (&self->impls, g_ptr_array_unref);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (xdp_settings_parent_class)->dispose (object);
}

static void
xdp_settings_class_init (XdpSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_settings_dispose;
}

static XdpSettings *
xdp_settings_new (GPtrArray *impls)
{
  XdpSettings *self;

  self = g_object_new (XDP_TYPE_SETTINGS, NULL);
  self->cancellable = g_cancellable_new ();
  self->impls = g_ptr_array_ref (impls);

  xdp_dbus_settings_set_version (XDP_DBUS_SETTINGS (self), 2);

  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_signal_connect_object (g_ptr_array_index (self->impls, i),
                               "setting-changed",
                               G_CALLBACK (on_impl_settings_changed),
                               self,
                               G_CONNECT_DEFAULT);
    }

  return self;
}

static GPtrArray *
create_impl_proxies (GDBusConnection *connection,
                     GPtrArray       *impl_configs)
{
  g_autoptr(GPtrArray) futures = g_ptr_array_new_with_free_func (dex_unref);
  g_autoptr(GPtrArray) impl_proxies =
    g_ptr_array_new_with_free_func (g_object_unref);

  for (size_t i = 0; i < impl_configs->len; i++)
    {
      XdpImplConfig *impl_config;
      g_autoptr(DexFuture) future = NULL;

      impl_config = g_ptr_array_index (impl_configs, i);
      future = xdp_dbus_impl_settings_proxy_new_future (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        impl_config->dbus_name,
                                                        DESKTOP_DBUS_PATH);
      g_ptr_array_add (futures, g_steal_pointer (&future));
    }

  dex_await (dex_future_allv ((DexFuture *const *) futures->pdata, futures->len), NULL);

  for (size_t i = 0; i < futures->len; i++)
    {
      DexFuture *future = g_ptr_array_index (futures, i);
      g_autoptr(GError) error = NULL;
      const GValue *value;

      value = dex_future_get_value (future, &error);
      if (value == NULL)
        g_warning ("Failed to create settings proxy: %s", error->message);
      else
        g_ptr_array_add (impl_proxies, g_object_ref (g_value_get_object (value)));
    }

  return g_steal_pointer (&impl_proxies);
}

DexFuture *
init_settings (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpSettings) settings = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  g_autoptr(GPtrArray) impl_configs = NULL;
  g_autoptr(GPtrArray) impl_proxies = NULL;

  impl_configs = xdp_portal_config_find_all (config, SETTINGS_DBUS_IMPL_IFACE);
  if (impl_configs->len == 0)
    return dex_future_new_true ();

  impl_proxies = create_impl_proxies (connection, impl_configs);

  if (impl_proxies->len == 0)
    {
      g_warning ("Not providing Settings portal: No working backend");
      return dex_future_new_false ();
    }

  settings = xdp_settings_new (impl_proxies);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&settings)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
