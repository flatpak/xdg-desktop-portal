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
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "xdp-portal-config.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

typedef struct _PortalInterface {
  /* dbus_name is NULL if this is the default */
  char *dbus_name;
  char **portals;
} PortalInterface;

typedef struct _PortalConfig {
  char *source;
  PortalInterface **interfaces;
  size_t n_ifaces;
  PortalInterface *default_portal;
} PortalConfig;

struct _XdpPortalConfig
{
  GObject parent_instance;

  char **current_desktops;
  GPtrArray *impls;
  PortalConfig *config;
};

G_DEFINE_FINAL_TYPE (XdpPortalConfig, xdp_portal_config, G_TYPE_OBJECT)

#define XDP_SUBDIR "xdg-desktop-portal"

static void
portal_interface_free (PortalInterface *iface)
{
  g_clear_pointer (&iface->dbus_name, g_free);
  g_clear_pointer (&iface->portals, g_strfreev);

  g_free (iface);
}

static void
portal_config_free (PortalConfig *config)
{
  g_clear_pointer (&config->source, g_free);

  for (size_t i = 0; i < config->n_ifaces; i++)
    portal_interface_free (config->interfaces[i]);

  g_clear_pointer (&config->default_portal, portal_interface_free);
  g_clear_pointer (&config->interfaces, g_free);

  g_free (config);
}

static void
portal_implementation_free (XdpPortalImplementation *impl)
{
  g_clear_pointer (&impl->source, g_free);
  g_clear_pointer (&impl->dbus_name, g_free);
  g_clear_pointer (&impl->interfaces, g_strfreev);
  g_clear_pointer (&impl->use_in, g_strfreev);
  g_free (impl);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdpPortalImplementation, portal_implementation_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalInterface, portal_interface_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalConfig, portal_config_free)

static const char **
xdp_portal_config_get_current_desktops (XdpPortalConfig *portal_config)
{
  return (const char **) portal_config->current_desktops;
}

/* Validation code taken from gdesktopappinfo.c {{{ */

/* See: https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html
 *
 * There's not much to go on: a desktop name must be composed of alphanumeric
 * characters, including '-' and '_'. Since we use this value to construct file
 * names, we are going to need avoid invalid characters
 */
static gboolean
validate_xdg_desktop (const char *desktop)
{
  size_t i;

  for (i = 0; desktop[i] != '\0'; i++)
    {
      if (desktop[i] != '-' &&
          desktop[i] != '_' &&
          !g_ascii_isalnum (desktop[i]))
        return FALSE;
    }

  if (i == 0)
    return FALSE;

  return TRUE;
}

static char **
get_valid_current_desktops (void)
{
  GPtrArray *valid_desktops;
  const char *value;
  char **tmp;

  value = g_getenv ("XDG_CURRENT_DESKTOP");
  if (value == NULL)
    value = "";

  tmp = g_strsplit (value, G_SEARCHPATH_SEPARATOR_S, 0);
  valid_desktops = g_ptr_array_new_full (g_strv_length (tmp) + 1, g_free);

  for (size_t i = 0; tmp[i] != NULL; i++)
    {
      if (validate_xdg_desktop (tmp[i]))
        g_ptr_array_add (valid_desktops, tmp[i]);
      else
        g_free (tmp[i]);
    }

  g_ptr_array_add (valid_desktops, NULL);
  g_free (tmp);

  tmp = (char **) g_ptr_array_steal (valid_desktops, NULL);
  g_ptr_array_unref (valid_desktops);

  return tmp;
}

static char **
get_current_lowercase_desktops (void)
{
  char **tmp = get_valid_current_desktops ();

  for (size_t i = 0; tmp[i] != NULL; i++)
    {
      /* Convert to lowercase */
      for (size_t j = 0; tmp[i][j] != '\0'; j++)
        tmp[i][j] = g_ascii_tolower (tmp[i][j]);
    }

  return tmp;
}
/* }}} */

static gboolean
register_portal (GHashTable  *portals,
                 const char  *path,
                 gboolean     opt_verbose,
                 GError     **error)
{
  g_autoptr(XdpPortalImplementation) impl = g_new0 (XdpPortalImplementation, 1);
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autofree char *basename = NULL;
  g_autofree char *source = NULL;
  int i;

  g_debug ("loading %s", path);

  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;

  basename = g_path_get_basename (path);
  source = g_strndup (basename, strrchr (basename, '.') - basename);
  impl->source = g_strdup (source);
  if (g_hash_table_contains (portals, impl->source))
    {
      g_debug ("Skipping duplicate source %s", source);
      return TRUE;
    }

  impl->dbus_name = g_key_file_get_string (keyfile, "portal", "DBusName", error);
  if (impl->dbus_name == NULL)
    return FALSE;

  if (!g_dbus_is_name (impl->dbus_name))
    {
      g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                   "Not a valid bus name: %s", impl->dbus_name);
      return FALSE;
    }

  impl->interfaces = g_key_file_get_string_list (keyfile, "portal", "Interfaces", NULL, error);
  if (impl->interfaces == NULL)
    return FALSE;

  for (i = 0; impl->interfaces[i]; i++)
    {
      if (!g_dbus_is_interface_name (impl->interfaces[i]))
        {
          g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                       "Not a valid interface name: %s", impl->interfaces[i]);
          return FALSE;
        }
      if (!g_str_has_prefix (impl->interfaces[i], "org.freedesktop.impl.portal."))
        {
          g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                       "Not a portal backend interface: %s", impl->interfaces[i]);
          return FALSE;
        }
    }

  if (opt_verbose)
    {
      for (i = 0; impl->interfaces[i]; i++)
        g_debug ("portal implementation supports %s", impl->interfaces[i]);
    }

  impl->use_in = g_key_file_get_string_list (keyfile, "portal", "UseIn", NULL, error);

  g_hash_table_insert (portals,
                       g_steal_pointer (&source),
                       g_steal_pointer (&impl));
  return TRUE;
}

static gboolean
g_strv_case_contains (const gchar * const *strv,
                      const gchar         *str)
{
  for (; strv && *strv != NULL; strv++)
    {
      if (g_ascii_strcasecmp (str, *strv) == 0)
        return TRUE;
    }

  return FALSE;
}

static gint
sort_impl_by_use_in_and_name (gconstpointer a,
                              gconstpointer b,
                              gpointer      user_data)
{
  const XdpPortalImplementation *pa = a;
  const XdpPortalImplementation *pb = b;
  XdpPortalConfig *portal_config = XDP_PORTAL_CONFIG (user_data);
  const char **desktops = xdp_portal_config_get_current_desktops (portal_config);
  int i;

  for (i = 0; desktops[i] != NULL; i++)
    {
      gboolean use_a = pa->use_in != NULL
                     ? g_strv_case_contains ((const char **)pa->use_in, desktops[i])
                     : FALSE;
      gboolean use_b = pb->use_in != NULL
                     ? g_strv_case_contains ((const char **)pb->use_in, desktops[i])
                     : FALSE;

      if (use_a != use_b)
        return use_b - use_a;
      else if (use_a)
        break;
      else
        continue;
    }

  return strcmp (pa->source, pb->source);
}

static void
load_installed_portals_dir (GHashTable *portals,
                            const char *portal_dir,
                            gboolean    opt_verbose)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GFile) dir = NULL;

  g_debug ("load portals from %s", portal_dir);

  dir = g_file_new_for_path (portal_dir);
  enumerator = g_file_enumerate_children (dir, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (enumerator == NULL)
    return;

  while (TRUE)
    {
      g_autoptr(GFileInfo) info = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GFile) child = NULL;
      g_autofree char *path = NULL;
      const char *name;

      info = g_file_enumerator_next_file (enumerator, NULL, NULL);

      if (info == NULL)
        break;

      name = g_file_info_get_name (info);

      if (!g_str_has_suffix (name, ".portal"))
        continue;

      child = g_file_enumerator_get_child (enumerator, info);
      path = g_file_get_path (child);

      if (!register_portal (portals, path, opt_verbose, &error))
        {
          g_warning ("Error loading %s: %s", path, error->message);
          continue;
        }
    }
}

static GPtrArray *
load_installed_portals (XdpPortalConfig *portal_config,
                        gboolean        opt_verbose)
{
  g_autoptr(GPtrArray) impls = NULL;
  g_autoptr (GHashTable) portals = NULL;
  const char *portal_dir;
  g_autofree char *user_portal_dir = NULL;
  const char * const *dirs;
  const char * const *iter;

  portals = g_hash_table_new_full (g_str_hash, g_str_equal,
                                   g_free,
                                   (GDestroyNotify) portal_implementation_free);

  /* We need to override this in the tests */
  portal_dir = g_getenv ("XDG_DESKTOP_PORTAL_DIR");
  if (portal_dir != NULL)
    {
      load_installed_portals_dir (portals, portal_dir, opt_verbose);
      /* All other config directories are ignored when this is set */
      goto out;
    }

  /* $XDG_DATA_HOME/xdg-desktop-portal/portals */
  user_portal_dir = g_build_filename (g_get_user_data_dir (), XDP_SUBDIR, "portals", NULL);
  load_installed_portals_dir (portals, user_portal_dir, opt_verbose);

  /* $XDG_DATA_DIRS/xdg-desktop-portal/portals */
  dirs = g_get_system_data_dirs ();

  for (iter = dirs; iter != NULL && *iter != NULL; iter++)
    {
      g_autofree char *dir = NULL;

      dir = g_build_filename (*iter, XDP_SUBDIR, "portals", NULL);
      load_installed_portals_dir (portals, dir, opt_verbose);
    }

  /* ${datadir}/xdg-desktop-portal/portals */
  portal_dir = DATADIR "/" XDP_SUBDIR "/portals";
  load_installed_portals_dir (portals, portal_dir, opt_verbose);

out:

  impls = g_hash_table_steal_all_values (portals);
  g_ptr_array_sort_values_with_data (impls,
                                     sort_impl_by_use_in_and_name,
                                     portal_config);
  return g_steal_pointer (&impls);
}

static PortalConfig *
load_portal_configuration_for_dir (gboolean    opt_verbose,
                                   const char *base_directory,
                                   const char *portal_file)
{
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *path = NULL;
  g_auto(GStrv) ifaces = NULL;

  key_file = g_key_file_new ();
  path = g_build_filename (base_directory, portal_file, NULL);

  g_debug ("Looking for portals configuration in '%s'", path);
  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL))
    return NULL;

  ifaces = g_key_file_get_keys (key_file, "preferred", NULL, NULL);

  if (ifaces != NULL)
    {
      g_autoptr(PortalInterface) default_portal = NULL;
      g_autoptr(PortalConfig) portal_config = NULL;
      g_autoptr(GPtrArray) interfaces = NULL;

      portal_config = g_new0 (PortalConfig, 1);
      interfaces = g_ptr_array_new_full (g_strv_length (ifaces) + 1, NULL);

      for (size_t i = 0; ifaces[i] != NULL; i++)
        {
          g_autoptr(PortalInterface) interface = g_new0 (PortalInterface, 1);

          interface->dbus_name = g_strdup (ifaces[i]);
          interface->portals = g_key_file_get_string_list (key_file, "preferred", ifaces[i], NULL, NULL);
          if (interface->portals == NULL)
            {
              g_critical ("Invalid portals for interface '%s' in %s", ifaces[i], portal_file);
              return NULL;
            }

          if (opt_verbose)
            {
              g_autofree char *preferred = g_strjoinv (", ", interface->portals);
              g_debug ("Preferred portals for interface '%s': %s", ifaces[i], preferred);
            }

          if (strcmp (ifaces[i], "default") == 0)
            {
              if (default_portal == NULL)
                default_portal = g_steal_pointer (&interface);
              else
                g_warning ("Duplicate default key will get ignored");
            }
          else
            {
              g_ptr_array_add (interfaces, g_steal_pointer (&interface));
            }
        }

      portal_config->n_ifaces = interfaces->len;
      portal_config->interfaces = (PortalInterface **) g_ptr_array_steal (interfaces, NULL);
      portal_config->default_portal = g_steal_pointer (&default_portal);

      return g_steal_pointer (&portal_config);
    }

  return NULL;
}

/*
 * Returns: %TRUE if configuration was found in @dir
 */
static PortalConfig *
load_config_directory (const char  *dir,
                       const char **desktops,
                       gboolean     opt_verbose)
{
  for (size_t i = 0; desktops[i] != NULL; i++)
    {
      g_autoptr(PortalConfig) config = NULL;
      g_autofree char *portals_conf = NULL;

      portals_conf = g_strdup_printf ("%s-portals.conf", desktops[i]);
      config = load_portal_configuration_for_dir (opt_verbose, dir, portals_conf);

      if (config != NULL)
        {
          if (opt_verbose)
            {
              g_debug ("Using portal configuration file '%s/%s' for desktop '%s'",
                       dir, portals_conf, desktops[i]);
            }

          return g_steal_pointer (&config);
        }
    }

  {
    g_autoptr(PortalConfig) config = NULL;

    config = load_portal_configuration_for_dir (opt_verbose, dir, "portals.conf");
    if (config != NULL)
      {
        if (opt_verbose)
          {
            g_debug ("Using portal configuration file '%s/%s' for non-specific desktop",
                     dir, "portals.conf");
          }

        return g_steal_pointer (&config);
      }
  }

  return NULL;
}

static PortalConfig *
load_portal_configuration (XdpPortalConfig *portal_config,
                           gboolean        opt_verbose)
{
  g_autoptr(PortalConfig) config = NULL;
  g_autofree char *user_portal_dir = NULL;
  const char * const *dirs;
  const char * const *iter;
  const char **desktops;
  const char *portal_dir;

  desktops = xdp_portal_config_get_current_desktops (portal_config);

  /* We need to override this in the tests */
  portal_dir = g_getenv ("XDG_DESKTOP_PORTAL_DIR");

  if (portal_dir != NULL)
    {
      /* All other config directories are ignored when this is set */
      return load_config_directory (portal_dir, desktops, opt_verbose);
    }

  /* $XDG_CONFIG_HOME/xdg-desktop-portal/(DESKTOP-)portals.conf */
  user_portal_dir = g_build_filename (g_get_user_config_dir (), XDP_SUBDIR, NULL);

  config = load_config_directory (user_portal_dir, desktops, opt_verbose);
  if (config)
    return g_steal_pointer (&config);

  /* $XDG_CONFIG_DIRS/xdg-desktop-portal/(DESKTOP-)portals.conf */
  dirs = g_get_system_config_dirs ();

  for (iter = dirs; iter != NULL && *iter != NULL; iter++)
    {
      g_autofree char *dir = g_build_filename (*iter, XDP_SUBDIR, NULL);

      config = load_config_directory (dir, desktops, opt_verbose);
      if (config)
        return g_steal_pointer (&config);
    }

  /* ${sysconfdir}/xdg-desktop-portal/(DESKTOP-)portals.conf */
  config = load_config_directory (SYSCONFDIR "/" XDP_SUBDIR, desktops, opt_verbose);
  if (config)
    return g_steal_pointer (&config);


  /* $XDG_DATA_HOME/xdg-desktop-portal/(DESKTOP-)portals.conf
   * (just for consistency with other XDG specifications) */
  g_clear_pointer (&user_portal_dir, g_free);
  user_portal_dir = g_build_filename (g_get_user_data_dir (), XDP_SUBDIR, NULL);

  config = load_config_directory (user_portal_dir, desktops, opt_verbose);
  if (config)
    return g_steal_pointer (&config);

  /* $XDG_DATA_DIRS/xdg-desktop-portal/(DESKTOP-)portals.conf */
  dirs = g_get_system_data_dirs ();

  for (iter = dirs; iter != NULL && *iter != NULL; iter++)
    {
      g_autofree char *dir = g_build_filename (*iter, XDP_SUBDIR, NULL);

      config = load_config_directory (dir, desktops, opt_verbose);
      if (config)
        return g_steal_pointer (&config);
    }

  /* ${datadir}/xdg-desktop-portal/(DESKTOP-)portals.conf */
  config = load_config_directory (DATADIR "/" XDP_SUBDIR, desktops, opt_verbose);
  if (config)
    return g_steal_pointer (&config);

  return NULL;
}

static void
xdp_portal_config_dispose (GObject *object)
{
  XdpPortalConfig *portal_config = XDP_PORTAL_CONFIG (object);

  g_clear_pointer (&portal_config->current_desktops, g_strfreev);
  g_clear_pointer (&portal_config->impls, g_ptr_array_unref);
  g_clear_pointer (&portal_config->config, portal_config_free);

  G_OBJECT_CLASS (xdp_portal_config_parent_class)->dispose (object);
}

static void
xdp_portal_config_class_init (XdpPortalConfigClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_portal_config_dispose;
}

static void
xdp_portal_config_init (XdpPortalConfig *portal_config)
{
}

XdpPortalConfig *
xdp_portal_config_new (gboolean opt_verbose)
{
  XdpPortalConfig *portal_config = g_object_new (XDP_TYPE_PORTAL_CONFIG, NULL);

  portal_config->current_desktops = get_current_lowercase_desktops ();
  portal_config->impls = load_installed_portals (portal_config, opt_verbose);
  portal_config->config = load_portal_configuration (portal_config, opt_verbose);

  return portal_config;
}

static PortalInterface *
find_matching_iface_config (XdpPortalConfig *portal_config,
                            const char      *interface)
{
  PortalConfig *config = portal_config->config;

  if (config == NULL)
    return NULL;

  for (size_t i = 0; i < config->n_ifaces; i++)
    {
      PortalInterface *iface = config->interfaces[i];

      if (g_strcmp0 (iface->dbus_name, interface) == 0)
        return iface;
    }

  return NULL;
}

static gboolean
portal_default_prefers_none (XdpPortalConfig *portal_config)
{
  PortalConfig *config = portal_config->config;

  if (config && config->default_portal &&
      g_strv_contains ((const char * const *) config->default_portal->portals, "none"))
    {
      g_debug ("Found 'none' in configuration for default");
      return TRUE;
    }

  return FALSE;
}

static gboolean
portal_interface_prefers_none (XdpPortalConfig *portal_config,
                               const char      *interface)
{
  const PortalInterface *iface =
    find_matching_iface_config (portal_config, interface);

  if (iface == NULL)
    return portal_default_prefers_none (portal_config);

  if (g_strv_contains ((const char * const *) iface->portals, "none"))
    {
      g_debug ("Found 'none' in configuration for %s", iface->dbus_name);
      return TRUE;
    }

  return FALSE;
}

static gboolean
portal_impl_supports_iface (const XdpPortalImplementation *impl,
                            const char                    *interface)
{
  return g_strv_contains ((const char * const *) impl->interfaces, interface);
}

static void
warn_please_use_portals_conf (void)
{
  g_warning_once ("The preferred method to match portal implementations "
                  "to desktop environments is to use the portals.conf(5) "
                  "configuration file");
}

static XdpPortalImplementation *
find_any_portal_implementation (XdpPortalConfig *portal_config,
                                const char      *interface)
{
  GPtrArray *implementations = portal_config->impls;

  for (size_t i = 0; i < implementations->len; i++)
    {
      XdpPortalImplementation *impl = g_ptr_array_index (implementations, i);

      if (!portal_impl_supports_iface (impl, interface))
        continue;

      g_debug ("Falling back to %s.portal for %s", impl->source, interface);
      return impl;
    }

  return NULL;
}

static XdpPortalImplementation *
find_portal_implementation_by_name (XdpPortalConfig *portal_config,
                                    const char      *portal_name)
{
  GPtrArray *implementations = portal_config->impls;

  if (portal_name == NULL)
    return NULL;

  for (size_t i = 0; i < implementations->len; i++)
    {
      XdpPortalImplementation *impl = g_ptr_array_index (implementations, i);

      if (g_str_equal (impl->source, portal_name))
        return impl;
    }

  g_debug ("Requested %s.portal is unrecognized", portal_name);
  return NULL;
}

static XdpPortalImplementation *
find_portal_implementation_iface (XdpPortalConfig       *portal_config,
                                  const PortalInterface *iface)
{
  if (iface == NULL)
    return NULL;

  for (size_t i = 0; iface->portals && iface->portals[i]; i++)
    {
      XdpPortalImplementation *impl;
      const char *portal = iface->portals[i];

      g_debug ("Found '%s' in configuration for %s", portal, iface->dbus_name);

      if (g_str_equal (portal, "*"))
        return find_any_portal_implementation (portal_config, iface->dbus_name);

      impl = find_portal_implementation_by_name (portal_config, portal);

      if (!impl)
        {
          g_info ("Requested backend %s does not exist. Skipping...", portal);
          continue;
        }

      if (!portal_impl_supports_iface (impl, iface->dbus_name))
        {
          g_info ("Requested backend %s.portal does not support %s. Skipping...", impl->source, iface->dbus_name);
          continue;
        }

      return impl;
    }

  return NULL;
}

static void
_add_all_portal_implementations_iface (XdpPortalConfig       *portal_config,
                                       const PortalInterface *iface,
                                       const char            *interface,
                                       GPtrArray             *impls)
{
  GPtrArray *implementations = portal_config->impls;
  g_autofree char *portals = NULL;

  portals = g_strjoinv (";", iface->portals);
  g_debug ("Found '%s' in configuration for %s", portals, iface->dbus_name);

  for (size_t i = 0; iface->portals && iface->portals[i]; i++)
    {
      const char *portal = iface->portals[i];

      for (size_t j = 0; j < implementations->len; j++)
        {
          XdpPortalImplementation *impl = g_ptr_array_index (implementations, j);

          if (!g_str_equal (impl->source, portal) && !g_str_equal (portal, "*"))
            continue;

          if (g_ptr_array_find (impls, impl, NULL))
            {
              g_info ("Duplicate backend %s.portal. Skipping...", impl->source);
              continue;
            }

          if (!portal_impl_supports_iface (impl, interface))
            {
              g_info ("Requested backend %s.portal does not support %s. Skipping...",
                      impl->source, interface);
              continue;
            }

          g_debug ("Using %s.portal for %s (config)", impl->source, interface);
          g_ptr_array_add (impls, impl);
        }
    }
}

static void
add_all_portal_implementations_iface (XdpPortalConfig       *portal_config,
                                      const PortalInterface *iface,
                                      GPtrArray             *impls)
{
  if (iface == NULL)
    return;

  _add_all_portal_implementations_iface (portal_config,
                                         iface,
                                         iface->dbus_name,
                                         impls);
}

static XdpPortalImplementation *
find_default_implementation_iface (XdpPortalConfig *portal_config,
                                   const char      *interface)
{
  PortalConfig *config = portal_config->config;
  PortalInterface *iface;

  if (config == NULL || config->default_portal == NULL)
    return NULL;

  iface = config->default_portal;

  for (size_t i = 0; iface->portals && iface->portals[i]; i++)
    {
      XdpPortalImplementation *impl;
      const char *portal = iface->portals[i];

      g_debug ("Found '%s' in configuration for default", portal);

      if (g_str_equal (portal, "*"))
        return find_any_portal_implementation (portal_config, iface->dbus_name);

      impl = find_portal_implementation_by_name (portal_config, portal);

      if (impl && portal_impl_supports_iface (impl, interface))
        return impl;
    }
  return NULL;
}

static void
add_all_default_portal_implementations_iface (XdpPortalConfig *portal_config,
                                              const char      *interface,
                                              GPtrArray       *impls)
{
  PortalConfig *config = portal_config->config;

  if (config == NULL || config->default_portal == NULL)
    return;

  _add_all_portal_implementations_iface (portal_config,
                                         config->default_portal,
                                         interface,
                                         impls);
}

static XdpPortalImplementation *
find_gtk_fallback_portal_implementation (XdpPortalConfig *portal_config,
                                         const char      *interface)
{
  /* As a last resort, if nothing was selected for this desktop by
   * ${desktop}-portals.conf or portals.conf, and no portal volunteered
   * itself as suitable for this desktop via the legacy UseIn mechanism,
   * try to fall back to x-d-p-gtk, which has historically been the portal
   * UI backend used by desktop environments with no backend of their own.
   * If it isn't installed, that is not an error: we just don't use it. */
  GPtrArray *implementations = portal_config->impls;

  for (size_t i = 0; i < implementations->len; i++)
    {
      XdpPortalImplementation *impl = g_ptr_array_index (implementations, i);

      if (!g_str_equal (impl->dbus_name, "org.freedesktop.impl.portal.desktop.gtk"))
        continue;

      if (!portal_impl_supports_iface (impl, interface))
        continue;

      g_warning ("Choosing %s.portal for %s as a last-resort fallback",
                 impl->source, interface);

      return impl;
    }

  return NULL;
}

XdpPortalImplementation *
xdp_portal_config_find_impl (XdpPortalConfig *portal_config,
                             const char      *interface)
{
  GPtrArray *implementations = portal_config->impls;
  PortalConfig *config = portal_config->config;
  const char **desktops = xdp_portal_config_get_current_desktops (portal_config);

  if (portal_interface_prefers_none (portal_config, interface))
    return NULL;

  if (config)
    {
      PortalInterface *iface =
        find_matching_iface_config (portal_config, interface);
      XdpPortalImplementation *impl =
        find_portal_implementation_iface (portal_config, iface);

      if (!impl)
        impl = find_default_implementation_iface (portal_config, interface);

      if (impl != NULL)
        {
          g_debug ("Using %s.portal for %s (config)", impl->source, interface);
          return impl;
        }
    }

  /* Fallback to the old UseIn key */
  for (size_t i = 0; desktops[i] != NULL; i++)
    {
      for (size_t j = 0; j < implementations->len; j++)
        {
          XdpPortalImplementation *impl = g_ptr_array_index (implementations, j);

          if (!portal_impl_supports_iface (impl, interface))
            continue;

          if (impl->use_in != NULL && g_strv_case_contains ((const char **)impl->use_in, desktops[i]))
            {
              g_warning ("Choosing %s.portal for %s via the deprecated UseIn key",
                         impl->source, interface);
              warn_please_use_portals_conf ();
              g_debug ("Using %s.portal for %s in %s (fallback)", impl->source, interface, desktops[i]);
              return impl;
            }
        }
    }

  return find_gtk_fallback_portal_implementation (portal_config, interface);
}

GPtrArray *
xdp_portal_config_find_all_impls (XdpPortalConfig *portal_config,
                                  const char      *interface)
{
  GPtrArray *implementations = portal_config->impls;
  const char **desktops;
  PortalInterface *iface;
  XdpPortalImplementation *gtk_fallback;
  g_autoptr(GPtrArray) impls = NULL;

  impls = g_ptr_array_new ();

  if (portal_interface_prefers_none (portal_config, interface))
    return g_steal_pointer (&impls);

  iface = find_matching_iface_config (portal_config, interface);
  add_all_portal_implementations_iface (portal_config, iface, impls);
  if (impls->len > 0)
    return g_steal_pointer (&impls);

  add_all_default_portal_implementations_iface (portal_config, interface, impls);
  if (impls->len > 0)
    return g_steal_pointer (&impls);

  desktops = xdp_portal_config_get_current_desktops (portal_config);

  /* Fallback to the old UseIn key */
  for (size_t i = 0; desktops[i] != NULL; i++)
    {
      for (size_t j = 0; j < implementations->len; j++)
        {
          XdpPortalImplementation *impl = g_ptr_array_index (implementations, j);

          if (!portal_impl_supports_iface (impl, interface))
            continue;

          if (impl->use_in != NULL && g_strv_case_contains ((const char **)impl->use_in, desktops[i]))
            {
              g_warning ("Choosing %s.portal for %s via the deprecated UseIn key",
                         impl->source, interface);
              warn_please_use_portals_conf ();
              g_debug ("Using %s.portal for %s in %s (fallback)", impl->source, interface, desktops[i]);
              g_ptr_array_add (impls, impl);
            }
        }
    }

  if (impls->len > 0)
    return g_steal_pointer (&impls);

  gtk_fallback = find_gtk_fallback_portal_implementation (portal_config,
                                                          interface);
  if (gtk_fallback)
    g_ptr_array_add (impls, gtk_fallback);

  return g_steal_pointer (&impls);
}
