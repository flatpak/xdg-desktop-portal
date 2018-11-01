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

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "settings.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

typedef struct _Settings Settings;
typedef struct _SettingsClass SettingsClass;

struct _Settings
{
  XdpSettingsSkeleton parent_instance;

  GHashTable *settings;
};

struct _SettingsClass
{
  XdpSettingsSkeletonClass parent_class;
};

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
namespace_matches (const char *namespace,
                   const char *pattern)
{
  size_t pattern_len;

  if (pattern[0] == '\0')
    return TRUE;
  if (strcmp (namespace, pattern) == 0)
    return TRUE;

  pattern_len = strlen (pattern);
  if (pattern[pattern_len - 1] == '*' && strncmp (namespace, pattern, pattern_len - 1) == 0)
    return TRUE;

  return FALSE;
}

static gboolean
settings_handle_read_all (XdpSettings           *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_namespace)
{
  Settings *self = (Settings *)object;
  GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(a{sa{sv}})"));
  GHashTableIter iter;
  char *key;
  SettingsBundle *value;

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sa{sv}}"));
  g_hash_table_iter_init (&iter, self->settings);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&value))
    {
      g_auto (GStrv) keys = NULL;
      GVariantDict dict;
      gsize i;

      if (!namespace_matches (key, arg_namespace))
        continue;

      keys = g_settings_schema_list_keys (value->schema);
      g_variant_dict_init (&dict, NULL);
      for (i = 0; keys[i]; ++i)
        g_variant_dict_insert_value (&dict, keys[i], g_settings_get_value (value->settings, keys[i]));

      g_variant_builder_add (&builder, "{s@a{sv}}", key, g_variant_dict_end (&dict));
    }
  g_variant_builder_close (&builder);

  g_dbus_method_invocation_return_value (invocation, g_variant_builder_end (&builder));
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

  // TODO: Handle kdeglobals via same interface
  if (!g_hash_table_contains (self->settings, arg_namespace))
    {
      g_debug ("Attempted to read from unknown namespace %s", arg_namespace);
      g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                     _("Requested setting not found"));
    }
  else
    {
      SettingsBundle *bundle = g_hash_table_lookup (self->settings, arg_namespace);
      if (g_settings_schema_has_key (bundle->schema, arg_key))
        {
          g_autoptr (GVariant) variant = NULL;
          variant = g_settings_get_value (bundle->settings, arg_key);
          g_dbus_method_invocation_return_value (invocation, g_variant_new("(v)", variant));
        }
      else
        {
          g_debug ("Attempted to read unknown key %s", arg_key);
          g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                         XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                         _("Requested setting not found"));
        }
    }

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
init_settings_table (Settings   *self,
                     GHashTable *table)
{
  static const char * const schemas[] = {
    "org.gnome.desktop.interface",
    "org.gnome.settings-daemon.peripherals.mouse",
    "org.gnome.desktop.sound",
    "org.gnome.desktop.privacy",
    "org.gnome.desktop.wm.preferences",
    "org.gnome.settings-daemon.plugins.xsettings",
    "org.gnome.desktop.a11y",
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

  xdp_settings_set_version (XDP_SETTINGS (self), 1);
}

static void
settings_finalize (GObject *object)
{
  Settings *self = (Settings*)object;
  g_hash_table_destroy (self->settings);

  G_OBJECT_CLASS (settings_parent_class)->finalize (object);
}

static void
settings_class_init (SettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = settings_finalize;
}

GDBusInterfaceSkeleton *
settings_create (GDBusConnection *connection)
{
  settings = g_object_new (settings_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (settings);
}
