/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "portal-impl.h"

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
  PortalInterface **ifaces;
  size_t n_ifaces;
  PortalInterface *dfl_portal;
} PortalConfig;

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
    portal_interface_free (config->ifaces[i]);

  g_clear_pointer (&config->dfl_portal, portal_interface_free);
  g_clear_pointer (&config->ifaces, g_free);

  g_free (config);
}

static void
portal_implementation_free (PortalImplementation *impl)
{
  g_clear_pointer (&impl->source, g_free);
  g_clear_pointer (&impl->dbus_name, g_free);
  g_clear_pointer (&impl->interfaces, g_strfreev);
  g_clear_pointer (&impl->use_in, g_strfreev);
  g_free (impl);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalImplementation, portal_implementation_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalInterface, portal_interface_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalConfig, portal_config_free)

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
get_valid_current_desktops (const char *value)
{
  char **tmp;
  GPtrArray *valid_desktops;

  if (value == NULL)
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

static const char **
get_current_lowercase_desktops (void)
{
  static char **result;

  if (g_once_init_enter (&result))
    {
      char **tmp = get_valid_current_desktops (NULL);

      for (size_t i = 0; tmp[i] != NULL; i++)
        {
          /* Convert to lowercase */
          for (size_t j = 0; tmp[i][j] != '\0'; j++)
            tmp[i][j] = g_ascii_tolower (tmp[i][j]);
        }

      g_once_init_leave (&result, tmp);
    }

  return (const char **) result;
}
/* }}} */

static PortalConfig *config = NULL;
static GList *implementations = NULL;

static gboolean
register_portal (const char  *path,
                 gboolean     opt_verbose,
                 GError     **error)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(PortalImplementation) impl = g_new0 (PortalImplementation, 1);
  g_autofree char *basename = NULL;
  int i;

  g_debug ("loading %s", path);

  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;

  basename = g_path_get_basename (path);
  impl->source = g_strndup (basename, strrchr (basename, '.') - basename);
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
  if (opt_verbose && impl->use_in != NULL)
    {
      g_autofree char *uses = g_strjoinv (", ", impl->use_in);
      g_warning ("Portal %s uses the deprecated UseIn key; the preferred method to "
                 "match portal implementations to desktop environments is to use the "
                 "portals.conf configuration file",
                 uses);
    }

  implementations = g_list_prepend (implementations, impl);
  impl = NULL;

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
                              gconstpointer b)
{
  const PortalImplementation *pa = a;
  const PortalImplementation *pb = b;
  const char **desktops;
  int i;

  desktops = get_current_lowercase_desktops ();

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

void
load_installed_portals (gboolean opt_verbose)
{
  const char *portal_dir;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) enumerator = NULL;

  /* We need to override this in the tests */
  portal_dir = g_getenv ("XDG_DESKTOP_PORTAL_DIR");
  if (portal_dir == NULL)
    portal_dir = DATADIR "/xdg-desktop-portal/portals";

  g_debug ("load portals from %s", portal_dir);

  dir = g_file_new_for_path (portal_dir);
  enumerator = g_file_enumerate_children (dir, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (enumerator == NULL)
    return;

  while (TRUE)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (enumerator, NULL, NULL);
      g_autoptr(GFile) child = NULL;
      g_autofree char *path = NULL;
      const char *name;
      g_autoptr(GError) error = NULL;

      if (info == NULL)
        break;

      name = g_file_info_get_name (info);

      if (!g_str_has_suffix (name, ".portal"))
        continue;

      child = g_file_enumerator_get_child (enumerator, info);
      path = g_file_get_path (child);

      if (!register_portal (path, opt_verbose, &error))
        {
          g_warning ("Error loading %s: %s", path, error->message);
          continue;
        }
    }

  implementations = g_list_sort (implementations, sort_impl_by_use_in_and_name);
}

static PortalConfig *
load_portal_configuration_for_dir (gboolean    opt_verbose,
                                   const char *base_directory,
                                   const char *portal_file)
{
  g_autofree char *path = g_build_filename (base_directory, portal_file, NULL);
  g_autoptr(GKeyFile) key_file = g_key_file_new ();

  g_debug ("Looking for portals configuration in '%s'", path);
  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL))
    return NULL;

  g_auto(GStrv) ifaces = g_key_file_get_keys (key_file, "preferred", NULL, NULL);

  if (ifaces != NULL)
    {
      g_autoptr(GPtrArray) interfaces = g_ptr_array_new_full (g_strv_length (ifaces) + 1, NULL);
      g_autoptr(PortalConfig) conf = g_new0 (PortalConfig, 1);
      g_autoptr(PortalInterface) dfl_portal = NULL;

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
            dfl_portal = g_steal_pointer (&interface);
          else
            g_ptr_array_add (interfaces, g_steal_pointer (&interface));
        }

      conf->n_ifaces = interfaces->len;
      conf->ifaces = (PortalInterface **) g_ptr_array_steal (interfaces, NULL);
      conf->dfl_portal = g_steal_pointer (&dfl_portal);

      return g_steal_pointer (&conf);
    }

  return NULL;
}

void
load_portal_configuration (gboolean opt_verbose)
{
  g_autoptr(PortalConfig) conf = NULL;
  g_autofree char *user_portal_dir = NULL;
  const char **desktops;
  const char *portal_dir;

  /* We need to override this in the tests */
  portal_dir = g_getenv ("XDG_DESKTOP_PORTAL_DIR");
  if (portal_dir == NULL)
    portal_dir = SYSCONFDIR "/xdg-desktop-portal";

  user_portal_dir = g_build_filename (g_get_user_config_dir (),
                                      "xdg-desktop-portal",
                                      NULL);

  conf = load_portal_configuration_for_dir (opt_verbose, user_portal_dir, "portals.conf");
  if (conf != NULL)
    {
      if (opt_verbose)
        g_debug ("Using user portal configuration file");

      config = g_steal_pointer (&conf);
    }

  desktops = get_current_lowercase_desktops ();
  for (size_t i = 0; desktops[i] != NULL; i++)
    {
      g_autofree char *portals_conf = g_strdup_printf ("%s-portals.conf", desktops[i]);

      conf = load_portal_configuration_for_dir (opt_verbose, user_portal_dir, portals_conf);
      if (conf != NULL)
        {
          if (opt_verbose)
            g_debug ("Using user portal configuration file '%s' for desktop '%s'",
                     portals_conf,
                     desktops[i]);

          config = g_steal_pointer (&conf);
          return;
        }

      conf = load_portal_configuration_for_dir (opt_verbose, portal_dir, portals_conf);
      if (conf != NULL)
        {
          if (opt_verbose)
            g_debug ("Using system portal configuration file '%s' for desktop '%s'",
                     portals_conf,
                     desktops[i]);

          config = g_steal_pointer (&conf);
          return;
        }
    }
}

static gboolean
portal_impl_name_matches (const PortalImplementation *impl,
                          const PortalInterface      *iface)
{
  /* Exact match */
  if (g_strv_contains ((const char * const *) iface->portals, impl->source))
    {
      g_debug ("Found '%s' in configuration for %s", impl->source, iface->dbus_name);
      return TRUE;
    }

  /* The "*" alias means "any" */
  if (g_strv_contains ((const char * const *) iface->portals, "*"))
    {
      g_debug ("Found '*' in configuration for %s", iface->dbus_name);
      return TRUE;
    }

  /* No portal */
  if (g_strv_contains ((const char * const *) iface->portals, "none"))
    {
      g_debug ("Found 'none' in configuration for %s", iface->dbus_name);
      return FALSE;
    }

  return FALSE;
}

static gboolean
portal_impl_matches_config (const PortalImplementation *impl,
                            const char                 *interface)
{
  if (config == NULL)
    return FALSE;

  /* Interfaces have precedence, followed by the "default" catch all,
   * to allow for specific interfaces to override the default
   */
  for (int i = 0; i < config->n_ifaces; i++)
    {
      const PortalInterface *iface = config->ifaces[i];

      if (g_strcmp0 (iface->dbus_name, interface) == 0)
        return portal_impl_name_matches (impl, iface);
    }

  if (config->dfl_portal)
    return portal_impl_name_matches (impl, config->dfl_portal);

  return FALSE;
}

PortalImplementation *
find_portal_implementation (const char *interface)
{
  const char **desktops;
  GList *l;
  int i;

  desktops = get_current_lowercase_desktops ();

  for (i = 0; desktops[i] != NULL; i++)
    {
     for (l = implementations; l != NULL; l = l->next)
        {
          PortalImplementation *impl = l->data;

          if (!g_strv_contains ((const char **)impl->interfaces, interface))
            continue;

          if (portal_impl_matches_config (impl, interface))
            {
              g_debug ("Using %s.portal for %s in %s (config)", impl->source, interface, desktops[i]);
              return impl;
            }
        }
    }

  /* Fallback to the old UseIn key */
  for (i = 0; desktops[i] != NULL; i++)
    {
     for (l = implementations; l != NULL; l = l->next)
        {
          PortalImplementation *impl = l->data;

          if (!g_strv_contains ((const char **)impl->interfaces, interface))
            continue;

          if (impl->use_in != NULL && g_strv_case_contains ((const char **)impl->use_in, desktops[i]))
            {
              g_debug ("Using %s.portal for %s in %s (fallback)", impl->source, interface, desktops[i]);
              return impl;
            }
        }
    }

#if 0
  /* Fall back to *any* installed implementation */
  for (l = implementations; l != NULL; l = l->next)
    {
      PortalImplementation *impl = l->data;

      if (!g_strv_contains ((const char **)impl->interfaces, interface))
        continue;

      g_debug ("Falling back to %s.portal for %s", impl->source, interface);
      return impl;
    }
#endif

  return NULL;
}

GPtrArray *
find_all_portal_implementations (const char *interface)
{
  GPtrArray *impls;
  GList *l;

  impls = g_ptr_array_new ();

  for (l = implementations; l != NULL; l = l->next)
    {
      PortalImplementation *impl = l->data;

      if (g_strv_contains ((const char **)impl->interfaces, interface))
        {
          g_debug ("Using %s.portal for %s", impl->source, interface);
          g_ptr_array_add (impls, impl);
        }
    }

  return impls;
}
