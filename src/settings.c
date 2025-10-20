/*
 * Copyright © 2018 Igalia S.L.
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
 *       Patrick Griffis <pgriffis@igalia.com>
 */

#include "config.h"

#include <time.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "xdp-dbus.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

#include "settings.h"

typedef struct _Settings Settings;
typedef struct _SettingsClass SettingsClass;

struct _Settings
{
  XdpDbusSettingsSkeleton parent_instance;
};

struct _SettingsClass
{
  XdpDbusSettingsSkeletonClass parent_class;
};

static XdpDbusImplSettings **impls;
static int n_impls = 0;

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
  g_autoptr(GHashTable) merged = NULL;
  g_autoptr(GVariant) settings = NULL;
  int j;

  merged = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  g_free,
                                  (GDestroyNotify) g_variant_dict_unref);

  for (j = n_impls - 1; j >= 0; j--)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_dbus_impl_settings_call_read_all_sync (impls[j], arg_namespaces,
                                                      &impl_value, NULL, &error))
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
  int i;

  g_debug ("Read %s %s", arg_namespace, arg_key);

  for (i = 0; i < n_impls; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_dbus_impl_settings_call_read_sync (impls[i], arg_namespace,
                                                  arg_key, &impl_value, NULL, &error))
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
  int i;

  g_debug ("ReadOne %s %s", arg_namespace, arg_key);

  for (i = 0; i < n_impls; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_dbus_impl_settings_call_read_sync (impls[i], arg_namespace,
                                                  arg_key, &impl_value, NULL, &error))
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

static void
on_impl_settings_changed (XdpDbusImplSettings *impl,
                          const char          *arg_namespace,
                          const char          *arg_key,
                          GVariant            *arg_value,
                          XdpDbusSettings     *settings)
{
  g_debug ("Emitting changed for %s %s", arg_namespace, arg_key);
  xdp_dbus_settings_emit_setting_changed (settings, arg_namespace,
                                          arg_key, arg_value);
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
  xdp_dbus_settings_set_version (XDP_DBUS_SETTINGS (settings), 2);
}

static void
settings_finalize (GObject *object)
{
  Settings *self = (Settings*)object;
  int i;

  for (i = 0; i < n_impls; i++)
    g_signal_handlers_disconnect_by_data (impls[i], self);

  G_OBJECT_CLASS (settings_parent_class)->finalize (object);
}

static void
settings_class_init (SettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = settings_finalize;
}

void
init_settings (XdpContext *context)
{
  g_autoptr(Settings) settings = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  g_autoptr(GPtrArray) impl_configs = NULL;
  g_autoptr(GError) error = NULL;

  impl_configs = xdp_portal_config_find_all (config,
                                             "org.freedesktop.impl.portal.Settings");
  if (impl_configs->len == 0)
    return;

  impls = g_new (XdpDbusImplSettings *, impl_configs->len);

  settings = g_object_new (settings_get_type (), NULL);

  for (size_t i = 0; i < impl_configs->len; i++)
    {
      XdpImplConfig *impl_config = g_ptr_array_index (impl_configs, i);
      const char *dbus_name = impl_config->dbus_name;

      XdpDbusImplSettings *impl_proxy =
        xdp_dbus_impl_settings_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               DESKTOP_PORTAL_OBJECT_PATH,
                                               NULL,
                                               &error);
      if (impl_proxy == NULL)
        {
          g_warning ("Failed to create settings proxy: %s", error->message);
        }
      else
        {
          impls[n_impls++] = impl_proxy;
          g_signal_connect (impl_proxy, "setting-changed",
                            G_CALLBACK (on_impl_settings_changed),
                            settings);
        }
    }

  if (n_impls == 0)
    return;

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&settings)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
