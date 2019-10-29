/*
 * Copyright © 2018 Igalia S.L.
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
 *       Patrick Griffis <pgriffis@igalia.com>
 */

#include "config.h"

#include <time.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "settings.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"
#include "fc-monitor.h"

typedef struct _Settings Settings;
typedef struct _SettingsClass SettingsClass;

struct _Settings
{
  XdpSettingsSkeleton parent_instance;

  GHashTable *settings;
  FcMonitor *fontconfig_monitor;
  int fontconfig_serial;
};

struct _SettingsClass
{
  XdpSettingsSkeletonClass parent_class;
};

static XdpImplSettings *impl;
static Settings *settings;

GType settings_get_type (void) G_GNUC_CONST;
static void settings_iface_init (XdpSettingsIface *iface);

G_DEFINE_TYPE_WITH_CODE (Settings, settings, XDP_TYPE_SETTINGS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SETTINGS, settings_iface_init));

typedef struct {
  GSettingsSchema *schema;
  GSettings *settings;
} SettingsBundle;

static SettingsBundle *
settings_bundle_new (GSettingsSchema *schema,
                     GSettings       *settings)
{
  SettingsBundle *bundle = g_new (SettingsBundle, 1);
  bundle->schema = schema;
  bundle->settings = settings;
  return bundle;
}

static void
settings_bundle_free (SettingsBundle *bundle)
{
  g_object_unref (bundle->schema);
  g_object_unref (bundle->settings);
  g_free (bundle);
}

static gboolean
namespace_matches (const char         *namespace,
                   const char * const *patterns)
{
  size_t i;

  for (i = 0; patterns[i]; ++i)
    {
      size_t pattern_len;
      const char *pattern = patterns[i];

      if (pattern[0] == '\0')
        return TRUE;
      if (strcmp (namespace, pattern) == 0)
        return TRUE;

      pattern_len = strlen (pattern);
      if (pattern[pattern_len - 1] == '*' && strncmp (namespace, pattern, pattern_len - 1) == 0)
        return TRUE;
    }

  if (i == 0) /* Empty array */
    return TRUE;

  return FALSE;
}

static gboolean
settings_handle_read_all (XdpSettings           *object,
                          GDBusMethodInvocation *invocation,
                          const char    * const *arg_namespaces)
{
  Settings *self = (Settings *)object;
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("(a{sa{sv}})"));
  GHashTableIter iter;
  char *key;
  SettingsBundle *value;

  g_variant_builder_open (builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  if (impl != NULL)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_impl_settings_call_read_all_sync (impl, arg_namespaces, &impl_value, NULL, &error))
        {
          g_warning ("Failed to ReadAll() from Settings implementation: %s", error->message);
        }
      else
        {
          size_t i;

          for (i = 0; i < g_variant_n_children (impl_value); ++i)
            {
              g_autoptr(GVariant) child = g_variant_get_child_value (impl_value, i);
              g_variant_builder_add_value (builder, child);
            }
        }
    }

  g_hash_table_iter_init (&iter, self->settings);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&value))
    {
      g_auto (GStrv) keys = NULL;
      GVariantDict dict;
      gsize i;

      if (!namespace_matches (key, arg_namespaces))
        continue;

      keys = g_settings_schema_list_keys (value->schema);
      g_variant_dict_init (&dict, NULL);
      for (i = 0; keys[i]; ++i)
        g_variant_dict_insert_value (&dict, keys[i], g_settings_get_value (value->settings, keys[i]));

      g_variant_builder_add (builder, "{s@a{sv}}", key, g_variant_dict_end (&dict));
    }

  if (namespace_matches ("org.gnome.fontconfig", arg_namespaces))
    {
      GVariantDict dict;

      g_variant_dict_init (&dict, NULL);
      g_variant_dict_insert_value (&dict, "serial", g_variant_new_int32 (self->fontconfig_serial));
      
      g_variant_builder_add (builder, "{s@a{sv}}", "org.gnome.fontconfig", g_variant_dict_end (&dict));
    }

  g_variant_builder_close (builder);

  g_dbus_method_invocation_return_value (invocation, g_variant_builder_end (builder));
  return TRUE;
}

static gboolean
settings_handle_read (XdpSettings           *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_namespace,
                      const char            *arg_key)
{
  Settings *self = (Settings *)object;

  g_debug ("Read %s %s", arg_namespace, arg_key);

  if (impl != NULL)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) impl_value = NULL;

      if (!xdp_impl_settings_call_read_sync (impl, arg_namespace, arg_key, &impl_value, NULL, &error))
        {
          /* A key not being found is expected, continue to our implementation */
          g_debug ("Failed to Read() from Settings implementation: %s", error->message);
        }
      else
        {
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", impl_value));
          return TRUE;
        }
    }

  if (strcmp (arg_namespace, "org.gnome.fontconfig") == 0)
    {
      if (strcmp (arg_key, "serial") == 0)
        {
          g_dbus_method_invocation_return_value (invocation,
                                                 g_variant_new ("(v)", g_variant_new_int32 (self->fontconfig_serial)));
          return TRUE;
        }
    }
  else if (g_hash_table_contains (self->settings, arg_namespace))
    {
      SettingsBundle *bundle = g_hash_table_lookup (self->settings, arg_namespace);
      if (g_settings_schema_has_key (bundle->schema, arg_key))
        {
          g_autoptr (GVariant) variant = NULL;
          variant = g_settings_get_value (bundle->settings, arg_key);
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", variant));
          return TRUE;
        }
    }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s", arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 _("Requested setting not found"));

  return TRUE;
}

typedef struct {
  Settings *self;
  const char *namespace;
} ChangedSignalUserData;

static ChangedSignalUserData *
changed_signal_user_data_new (Settings   *settings,
                              const char *namespace)
{
  ChangedSignalUserData *data = g_new (ChangedSignalUserData, 1);
  data->self = settings;
  data->namespace = namespace;
  return data;
}

static void
changed_signal_user_data_destroy (gpointer  data,
                                  GClosure *closure)
{
  g_free (data);
}

static void
on_settings_changed (GSettings             *settings,
                     const char            *key,
                     ChangedSignalUserData *user_data)
{
  g_autoptr (GVariant) new_value = g_settings_get_value (settings, key);

  g_debug ("Emitting changed for %s %s", user_data->namespace, key);
  xdp_settings_emit_setting_changed (XDP_SETTINGS (user_data->self), user_data->namespace, key, g_variant_new ("v", new_value));
}

static void
on_impl_settings_changed (XdpImplSettings *impl,
                          const char      *arg_namespace,
                          const char      *arg_key,
                          GVariant        *arg_value,
                          XdpSettings     *settings)
{
  g_debug ("Emitting changed for %s %s", arg_namespace, arg_key);
  xdp_settings_emit_setting_changed (settings, arg_namespace, arg_key, arg_value);
}

static void
init_settings_table (Settings   *self,
                     GHashTable *table)
{
  static const char * const schemas[] = {
    "org.gnome.desktop.a11y",
    "org.gnome.desktop.input-sources",
    "org.gnome.desktop.interface",
    "org.gnome.desktop.privacy",
    "org.gnome.desktop.sound",
    "org.gnome.desktop.wm.preferences",
    "org.gnome.settings-daemon.peripherals.keyboard",
    "org.gnome.settings-daemon.peripherals.mouse",
    "org.gnome.settings-daemon.plugins.xsettings",
  };
  size_t i;
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();

  for (i = 0; i < G_N_ELEMENTS(schemas); ++i)
    {
      GSettings *setting;
      GSettingsSchema *schema;
      SettingsBundle *bundle;
      const char *schema_name = schemas[i];
      
      schema = g_settings_schema_source_lookup (source, schema_name, TRUE);
      if (!schema)
        {
          g_debug ("%s schema not found", schema_name);
          continue;
        }

      setting = g_settings_new (schema_name);
      bundle = settings_bundle_new (schema, setting);
      g_signal_connect_data (setting, "changed", G_CALLBACK(on_settings_changed),
                             changed_signal_user_data_new (self, schema_name),
                             changed_signal_user_data_destroy, 0);
      g_hash_table_insert (table, (char*)schema_name, bundle);
    }
}

static void
fontconfig_changed (FcMonitor *monitor,
                    Settings *self)
{
  const char *namespace = "org.gnome.fontconfig";
  const char *key = "serial";
  
  g_debug ("Emitting changed for %s %s", namespace, key);

  self->fontconfig_serial++;

  xdp_settings_emit_setting_changed (XDP_SETTINGS (self),
                                     namespace, key,
                                     g_variant_new ("v", g_variant_new_int32 (self->fontconfig_serial)));
}

static void
settings_iface_init (XdpSettingsIface *iface)
{
  iface->handle_read = settings_handle_read;
  iface->handle_read_all = settings_handle_read_all;
}

static void
settings_init (Settings *self)
{
  self->settings = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)settings_bundle_free);
  init_settings_table (self, self->settings);

  self->fontconfig_monitor = fc_monitor_new ();
  g_signal_connect (self->fontconfig_monitor, "updated", G_CALLBACK (fontconfig_changed), self);
  fc_monitor_start (self->fontconfig_monitor);

  xdp_settings_set_version (XDP_SETTINGS (self), 1);
}

static void
settings_finalize (GObject *object)
{
  Settings *self = (Settings*)object;
  g_hash_table_destroy (self->settings);

  g_signal_handlers_disconnect_by_data (impl, self);
  g_signal_handlers_disconnect_by_data (self->fontconfig_monitor, self);
  fc_monitor_stop (self->fontconfig_monitor);
  g_object_unref (self->fontconfig_monitor);

  G_OBJECT_CLASS (settings_parent_class)->finalize (object);
}

static void
settings_class_init (SettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = settings_finalize;
}

GDBusInterfaceSkeleton *
settings_create (GDBusConnection *connection,
                 const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  if (dbus_name != NULL)
    {
      impl = xdp_impl_settings_proxy_new_sync (connection,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              dbus_name,
                                              DESKTOP_PORTAL_OBJECT_PATH,
                                              NULL,
                                              &error);
      if (impl == NULL)
        {
          g_warning ("Failed to create settings proxy: %s", error->message);
        }
    }

  settings = g_object_new (settings_get_type (), NULL);

  if (impl != NULL)
    g_signal_connect (impl, "setting-changed", G_CALLBACK (on_impl_settings_changed), settings);

  return G_DBUS_INTERFACE_SKELETON (settings);
}
