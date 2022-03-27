/*
 * Copyright © 2022 Matthew Leeds
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
 *       Matthew Leeds <mwleeds@protonmail.com>
 */

#include "config.h"

#ifdef HAVE_GLIB_2_66

#include <glib.h>
#include <gio/gdesktopappinfo.h>

#include "dynamic-launcher.h"
#include "xdp-utils.h"

static char *
find_renamed_app_id (const char *old_app_id)
{
  g_autofree char *renamed_to = NULL;
  g_autofree char *desktop_id = NULL;

  desktop_id = g_strconcat (old_app_id, ".desktop", NULL);

  GList *app_infos = g_app_info_get_all ();
  for (GList *l = app_infos; l; l = l->next)
    {
      GDesktopAppInfo *info = l->data;
      g_auto(GStrv) renamed_from = NULL;
      renamed_from = g_desktop_app_info_get_string_list (info, "X-Flatpak-RenamedFrom", NULL);
      if (renamed_from == NULL)
        continue;
      if (!g_strv_contains ((const char * const *)renamed_from, desktop_id))
        continue;

      renamed_to = g_desktop_app_info_get_string (info, "X-Flatpak");
      break;
    }

  g_list_free_full (app_infos, g_object_unref);
  return g_steal_pointer (&renamed_to);
}

/*
 * It's possible an app was renamed using Flatpak's end-of-life-rebase
 * mechanism, and either (a) the app was installed system-wide and the update
 * was applied by another user, so the migration for this user has to happen
 * when this binary runs at the start of the session, or (b) the version of
 * Flatpak is not new enough for the migration of the launchers to be handled
 * by Flatpak, so x-d-p has to do it.
 *
 * This function also handles deleting the launchers in case the parent app has
 * been uninstalled.
 */
static gboolean
migrate_renamed_app_launchers (void)
{
  g_autoptr(GFile) desktop_dir = NULL;
  g_autoptr(GFileEnumerator) children = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *icon_dir_path = NULL;
  g_autofree char *desktop_dir_path = NULL;
  gboolean success = TRUE;

  desktop_dir_path = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_APPLICATIONS_DIR, NULL);
  desktop_dir = g_file_new_for_path (desktop_dir_path);
  children = g_file_enumerate_children (desktop_dir, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
  if (children == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_warning ("Error encountered enumerating launchers: %s", error->message);
          success = FALSE;
        }
      return success;
    }

  icon_dir_path = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_ICONS_DIR, NULL);
  for (;;)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (children, NULL, NULL);
      const char *desktop_name;
      g_autofree char *desktop_path = NULL;
      g_autofree char *icon_path = NULL;
      g_autofree char *tryexec_path = NULL;
      g_autofree char *app_id = NULL;
      g_autofree char *renamed_to = NULL;
      g_autoptr(GKeyFile) key_file = NULL;
      g_autoptr(GFile) icon_file = NULL;
      g_autoptr(GFile) desktop_file = NULL;

      if (info == NULL)
        return success;

      desktop_name = g_file_info_get_name (info);
      if (!g_str_has_suffix (desktop_name, ".desktop"))
        continue;

      desktop_path = g_build_filename (g_file_peek_path (desktop_dir), desktop_name, NULL);
      key_file = g_key_file_new ();
      if (!g_key_file_load_from_file (key_file, desktop_path,
                                      G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                      &error))
        {
          g_warning ("Error encountered loading key file %s: %s", desktop_path, error->message);
          g_clear_error (&error);
          success = FALSE;
          continue;
        }

      tryexec_path = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "TryExec", NULL);
      if (tryexec_path == NULL)
        continue;
      if (!g_path_is_absolute (tryexec_path))
        {
          /* Here we're just checking for existence not a renamed binary */
          renamed_to = g_find_program_in_path (tryexec_path);
          if (renamed_to)
            continue;
        }
      else if (g_file_test (tryexec_path, G_FILE_TEST_IS_EXECUTABLE))
        {
          continue;
        }
      else if (g_key_file_has_key (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL))
        {
          app_id = g_path_get_basename (tryexec_path);
          if (strlen(app_id) < 2 ||
              !g_dbus_is_name (app_id) ||
              !g_str_has_prefix (desktop_name, app_id))
            {
              g_warning ("Unable to determine app id for %s", desktop_name);
              success = FALSE;
              continue;
            }
          renamed_to = find_renamed_app_id (app_id);
        }

      icon_path = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "Icon", NULL);
      if (g_str_has_prefix (icon_path, icon_dir_path))
        icon_file = g_file_new_for_path (icon_path);

      desktop_file = g_file_new_for_path (desktop_path);

      if (renamed_to == NULL)
        {
          g_autoptr(GFile) link_file = NULL;

          link_file = g_file_new_build_filename (g_get_user_data_dir (), "applications", desktop_name, NULL);
          if (!g_file_delete (link_file, NULL, &error))
            {
              g_warning ("Couldn't delete sym link %s: %s", g_file_peek_path (link_file), error->message);
              g_clear_error (&error);
              success = FALSE;
            }

          if (!g_file_delete (desktop_file, NULL, &error))
            {
              g_warning ("Couldn't delete desktop file %s: %s", g_file_peek_path (desktop_file), error->message);
              g_clear_error (&error);
              success = FALSE;
            }

          if (icon_file && !g_file_delete (icon_file, NULL, &error))
            {
              g_warning ("Couldn't delete icon file %s: %s", g_file_peek_path (icon_file), error->message);
              g_clear_error (&error);
              success = FALSE;
            }
        }
      else /* renamed_to != NULL */
        {
          g_autoptr(GFile) link_file = NULL;
          g_autoptr(GFile) new_link_file = NULL;
          g_autoptr(GString) data_string = NULL;
          g_autoptr(GKeyFile) new_key_file = NULL;
          g_autofree char *new_desktop = NULL;
          g_autofree char *new_desktop_path = NULL;
          g_autofree char *new_icon = NULL;
          g_autofree char *icon_basename = NULL;
          g_autofree char *link_path = NULL;
          g_autofree char *relative_path = NULL;
          g_autofree char *old_data = NULL;
          const gchar *desktop_suffix;
          gchar *icon_suffix;

          if (!g_key_file_has_key (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL))
            g_assert_not_reached ();

          /* Fix paths in desktop file with a find-and-replace. */
          old_data = g_key_file_to_data (key_file, NULL, NULL);
          data_string = g_string_new ((const char *)old_data);
          g_string_replace (data_string, app_id, renamed_to, 0);
          new_key_file = g_key_file_new ();
          if (!g_key_file_load_from_data (new_key_file, data_string->str, -1,
                                          G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                          &error))
            {
              g_warning ("Cannot load desktop file %s after rewrite: %s", desktop_path, error->message);
              g_warning ("Key file contents:\n%s\n", (const char *)data_string->str);
              g_clear_error (&error);
              success = FALSE;
              continue;
            }

          /* Write it out at the new path */
          g_assert (g_str_has_prefix (desktop_name, app_id));
          desktop_suffix = desktop_name + strlen (app_id);
          new_desktop = g_strconcat (renamed_to, desktop_suffix, NULL);
          new_desktop_path = g_build_filename (desktop_dir_path, new_desktop, NULL);
          if (!g_key_file_save_to_file (new_key_file, new_desktop_path, &error))
            {
              g_warning ("Couldn't rewrite desktop file %s to %s: %s",
                         desktop_path, new_desktop_path, error->message);
              g_clear_error (&error);
              success = FALSE;
              continue;
            }

          /* Fix symlink */
          link_path = g_build_filename (g_get_user_data_dir (), "applications", desktop_name, NULL);
          link_file = g_file_new_for_path (link_path);
          relative_path = g_build_filename ("..", XDG_PORTAL_APPLICATIONS_DIR, new_desktop, NULL);
          g_file_delete (link_file, NULL, NULL);
          new_link_file = g_file_new_build_filename (g_get_user_data_dir (), "applications", new_desktop, NULL);
          if (!g_file_make_symbolic_link (new_link_file, relative_path, NULL, &error))
            {
              g_warning ("Unable to rename desktop file link %s -> %s: %s",
                         desktop_name, new_desktop, error->message);
              g_clear_error (&error);
              success = FALSE;
              continue;
            }

          /* Delete the old desktop file */
          unlink (desktop_path);

          /* And rename the icon */
          if (icon_file)
            {
              icon_basename = g_path_get_basename (icon_path);
              if (!g_str_has_prefix (icon_basename, app_id))
                continue;

              icon_suffix = icon_basename + strlen (app_id);
              new_icon = g_strconcat (renamed_to, icon_suffix, NULL);
              if (!g_file_set_display_name (icon_file, new_icon, NULL, &error))
                {
                  g_warning ("Unable to rename icon file %s -> %s: %s",
                             icon_basename, new_icon, error->message);
                  g_clear_error (&error);
                  success = FALSE;
                  continue;
                }
            }
        }
    }

  return success;
}
#endif /* HAVE_GLIB_2_66 */

int
main (int argc, char *argv[])
{
/* The dynamic launcher portal is only compiled against GLib >= 2.66 */
#ifdef HAVE_GLIB_2_66
  if (!migrate_renamed_app_launchers ())
    return 1;
#endif
  return 0;
}
