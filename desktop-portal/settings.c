/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "settings.h"

#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <glib/gi18n.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

typedef struct _Settings Settings;
typedef struct _SettingsClass SettingsClass;

struct _Settings
{
  XdpDbusSettingsSkeleton parent_instance;

  XdpDbusImplSettings **impls;
  size_t n_impls;

  GHashTable *last_emitted; /* "namespace\x1fkey" -> GVariant */
  GCancellable *cancellable;
};

struct _SettingsClass
{
  XdpDbusSettingsSkeletonClass parent_class;
};

GType settings_get_type (void) G_GNUC_CONST;
static void settings_iface_init (XdpDbusSettingsIface *iface);

G_DEFINE_TYPE_WITH_CODE (Settings, settings, XDP_DBUS_TYPE_SETTINGS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SETTINGS,
                                                settings_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Settings, g_object_unref)

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
  Settings *self = (Settings*)object;
  g_autoptr(GHashTable) merged = NULL;
  g_autoptr(GVariant) settings = NULL;

  merged = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  g_free,
                                  (GDestroyNotify) g_variant_dict_unref);

  for (size_t i = 0; i < self->n_impls; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;
      size_t j = self->n_impls - i - 1;

      if (!xdp_dbus_impl_settings_call_read_all_sync (self->impls[j],
                                                      arg_namespaces,
                                                      &impl_value,
                                                      NULL, &error))
        g_warning ("Failed to ReadAll() from Settings implementation: %s", error->message);
      else
        merge_impl_settings (merged, impl_value);
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
  Settings *self = (Settings*)object;

  g_debug ("Read %s %s", arg_namespace, arg_key);

  for (size_t i = 0; i < self->n_impls; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_dbus_impl_settings_call_read_sync (self->impls[i],
                                                  arg_namespace,
                                                  arg_key,
                                                  &impl_value,
                                                  NULL, &error))
        {
          /* A key not being found is expected, continue to our implementation */
          g_debug ("Failed to Read() from Settings implementation: %s", error->message);
        }
      else
        {
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", impl_value));
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s", arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
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
  Settings *self = (Settings*)object;

  g_debug ("ReadOne %s %s", arg_namespace, arg_key);

  for (size_t i = 0; i < self->n_impls; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_dbus_impl_settings_call_read_sync (self->impls[i],
                                                  arg_namespace,
                                                  arg_key,
                                                  &impl_value,
                                                  NULL, &error))
        {
          /* A key not being found is expected, continue to our implementation */
          g_debug ("Failed to Read() from Settings implementation: %s", error->message);
        }
      else
        {
          g_dbus_method_invocation_return_value (invocation, g_variant_new_tuple (&impl_value, 1));
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s", arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 _("Requested setting not found"));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

typedef struct
{
  Settings *self;
  char *ns;
  char *key;
  size_t impl_idx;
} ResolveData;

static void
resolve_data_free (ResolveData *data)
{
  if (data == NULL)
    return;

  g_object_unref (data->self);
  g_free (data->ns);
  g_free (data->key);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ResolveData, resolve_data_free)

static ResolveData *
resolve_data_new (Settings   *self,
                  const char *ns,
                  const char *key)
{
  ResolveData *data = g_new0 (ResolveData, 1);
  data->self = g_object_ref (self);
  data->ns = g_strdup (ns);
  data->key = g_strdup (key);
  return data;
}

static void settings_resolve_read_cb (GObject      *source,
                                      GAsyncResult *result,
                                      gpointer      user_data);

static void
settings_resolve_next (ResolveData *owned_data)
{
  g_autoptr(ResolveData) data = owned_data;
  XdpDbusImplSettings *impl;
  GCancellable *cancellable;
  const char *ns;
  const char *key;

  if (data->impl_idx >= data->self->n_impls)
    return;

  impl = data->self->impls[data->impl_idx];
  cancellable = data->self->cancellable;
  ns = data->ns;
  key = data->key;

  xdp_dbus_impl_settings_call_read (impl, ns, key, cancellable,
                                    settings_resolve_read_cb,
                                    g_steal_pointer (&data));
}

static void
settings_resolve_read_cb (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(ResolveData) data = user_data;
  g_autoptr(GVariant) resolved = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *cache_key = NULL;
  GVariant *last_value;

  if (!xdp_dbus_impl_settings_call_read_finish (XDP_DBUS_IMPL_SETTINGS (source),
                                                &resolved,
                                                result,
                                                &error))
    {
      data->impl_idx++;
      settings_resolve_next (g_steal_pointer (&data));
      return;
    }

  cache_key = g_strconcat (data->ns, "\x1f", data->key, NULL);
  last_value = g_hash_table_lookup (data->self->last_emitted, cache_key);

  if (last_value == NULL || !g_variant_equal (resolved, last_value))
    {
      g_debug ("Emitting changed for %s %s", data->ns, data->key);

      xdp_dbus_settings_emit_setting_changed (XDP_DBUS_SETTINGS (data->self),
                                              data->ns, data->key,
                                              resolved);

      g_hash_table_insert (data->self->last_emitted,
                           g_steal_pointer (&cache_key),
                           g_steal_pointer (&resolved));
    }
}

static void
on_impl_settings_changed (XdpDbusImplSettings *impl,
                          const char          *arg_namespace,
                          const char          *arg_key,
                          GVariant            *arg_value,
                          XdpDbusSettings     *settings)
{
  Settings *self = (Settings *)settings;

  settings_resolve_next (resolve_data_new (self, arg_namespace, arg_key));
}

static void
settings_iface_init (XdpDbusSettingsIface *iface)
{
  iface->handle_read = settings_handle_read;
  iface->handle_read_one = settings_handle_read_one;
  iface->handle_read_all = settings_handle_read_all;
}

static void
settings_init (Settings *settings)
{
}

static void
settings_finalize (GObject *object)
{
  Settings *self = (Settings*)object;

  for (size_t i = 0; i < self->n_impls; i++)
    {
      g_signal_handlers_disconnect_by_data (self->impls[i], self);
      g_clear_object (&self->impls[i]);
    }

  g_clear_pointer(&self->impls, g_free);
  g_clear_pointer (&self->last_emitted, g_hash_table_unref);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (settings_parent_class)->finalize (object);
}

static void
settings_class_init (SettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = settings_finalize;
}

static Settings *
settings_new (GPtrArray *impls)
{
  Settings *settings;

  settings = g_object_new (settings_get_type (), NULL);
  settings->n_impls = impls->len;
  settings->impls = (XdpDbusImplSettings **) g_ptr_array_steal (impls, NULL);
  settings->last_emitted = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free,
                                                  (GDestroyNotify) g_variant_unref);
  settings->cancellable = g_cancellable_new ();

  xdp_dbus_settings_set_version (XDP_DBUS_SETTINGS (settings), 2);

  for (size_t i = 0; i < settings->n_impls; i++)
    {
      g_signal_connect_object (settings->impls[i], "setting-changed",
                               G_CALLBACK (on_impl_settings_changed),
                               settings,
                               G_CONNECT_DEFAULT);
    }

  return settings;
}

void
init_settings (XdpContext *context)
{
  g_autoptr(Settings) settings = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  g_autoptr(GPtrArray) impl_configs = NULL;
  g_autoptr(GPtrArray) impl_proxies = NULL;

  impl_configs = xdp_portal_config_find_all (config, SETTINGS_DBUS_IMPL_IFACE);
  if (impl_configs->len == 0)
    return;

  impl_proxies = g_ptr_array_new_with_free_func (g_object_unref);

  for (size_t i = 0; i < impl_configs->len; i++)
    {
      XdpImplConfig *impl_config = g_ptr_array_index (impl_configs, i);
      const char *dbus_name = impl_config->dbus_name;
      g_autoptr(XdpDbusImplSettings) impl_proxy = NULL;
      g_autoptr(GError) error = NULL;

      impl_proxy =
        xdp_dbus_impl_settings_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               DESKTOP_DBUS_PATH,
                                               NULL,
                                               &error);
      if (impl_proxy == NULL)
        g_warning ("Failed to create settings proxy: %s", error->message);
      else
        g_ptr_array_add (impl_proxies, g_steal_pointer (&impl_proxy));
    }

  if (impl_proxies->len == 0)
    {
      g_warning ("Not providing Settings portal: No working backend");
      return;
    }

  settings = settings_new (impl_proxies);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&settings)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
