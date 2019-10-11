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

#include "settings.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Settings Settings;
typedef struct _SettingsClass SettingsClass;

struct _Settings
{
  XdpSettingsSkeleton parent_instance;
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

static gboolean
settings_handle_read_all (XdpSettings           *object,
                          GDBusMethodInvocation *invocation,
                          const char    * const *arg_namespaces)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) impl_value = NULL;
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("(a{sa{sv}})"));

  g_variant_builder_open (builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  if (!xdp_impl_settings_call_read_all_sync (impl, arg_namespaces,
                                             &impl_value, NULL, &error))
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
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) impl_value = NULL;

  g_debug ("Read %s %s", arg_namespace, arg_key);

  if (!xdp_impl_settings_call_read_sync (impl,
                                         arg_namespace, arg_key,
                                         &impl_value, NULL, &error))
    {
      /* A key not being found is expected, continue to our implementation */
      g_debug ("Failed to Read() from Settings implementation: %s", error->message);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", impl_value));
      return TRUE;
    }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s", arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 _("Requested setting not found"));

  return TRUE;
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
settings_iface_init (XdpSettingsIface *iface)
{
  iface->handle_read = settings_handle_read;
  iface->handle_read_all = settings_handle_read_all;
}

static void
settings_init (Settings *self)
{
  xdp_settings_set_version (XDP_SETTINGS (self), 1);
}

static void
settings_finalize (GObject *object)
{
  Settings *self = (Settings*)object;

  g_signal_handlers_disconnect_by_data (impl, self);

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

  impl = xdp_impl_settings_proxy_new_sync (connection,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           dbus_name,
                                           DESKTOP_PORTAL_OBJECT_PATH,
                                           NULL,
                                           &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create settings proxy: %s", error->message);
      return NULL;
    }

  settings = g_object_new (settings_get_type (), NULL);

  g_signal_connect (impl, "setting-changed", G_CALLBACK (on_impl_settings_changed), settings);

  return G_DBUS_INTERFACE_SKELETON (settings);
}
