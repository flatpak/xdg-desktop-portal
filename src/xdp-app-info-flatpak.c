/*
 * Copyright © 2024 Red Hat, Inc
 * Copyright © 2024 GNOME Foundation Inc.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Hubert Figuière <hub@figuiere.net>
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <json-glib/json-glib.h>

#include "xdp-app-info-flatpak-private.h"
#include "xdp-usb-query.h"

#define FLATPAK_ENGINE_ID "org.flatpak"

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_KEY_APP_PATH "app-path"
#define FLATPAK_METADATA_KEY_ORIGINAL_APP_PATH "original-app-path"
#define FLATPAK_METADATA_KEY_RUNTIME_PATH "runtime-path"
#define FLATPAK_METADATA_KEY_INSTANCE_ID "instance-id"
#define FLATPAK_METADATA_GROUP_CONTEXT "Context"
#define FLATPAK_METADATA_KEY_SHARED "shared"
#define FLATPAK_METADATA_CONTEXT_SHARED_NETWORK "network"
#define FLATPAK_METADATA_GROUP_RUNTIME "Runtime"

struct _XdpAppInfoFlatpak
{
  XdpAppInfo parent;

  GKeyFile *flatpak_info;
  GPtrArray *queries;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoFlatpak, xdp_app_info_flatpak, XDP_TYPE_APP_INFO)

static gboolean
is_valid_initial_name_character (gint c, gboolean allow_dash)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_') || (allow_dash && c == '-');
}

static gboolean
is_valid_name_character (gint c, gboolean allow_dash)
{
  return
    is_valid_initial_name_character (c, allow_dash) ||
    (c >= '0' && c <= '9');
}

static const char *
find_last_char (const char *str, gsize len, int c)
{
  const char *p = str + len - 1;
  while (p >= str)
    {
      if (*p == c)
        return p;
      p--;
    }
  return NULL;
}

/**
 * flatpak_is_valid_name:
 * @string: The string to check
 *
 * Checks if @string is a valid application name.
 *
 * App names are composed of 3 or more elements separated by a period
 * ('.') character. All elements must contain at least one character.
 *
 * Each element must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_-". Elements may not begin with a digit.
 * Additionally "-" is only allowed in the last element.
 *
 * App names must not begin with a '.' (period) character.
 *
 * App names must not exceed 255 characters in length.
 *
 * The above means that any app name is also a valid DBus well known
 * bus name, but not all DBus names are valid app names. The difference are:
 * 1) DBus name elements may contain '-' in the non-last element.
 * 2) DBus names require only two elements
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 */
static gboolean
flatpak_is_valid_name (const char *string)
{
  gssize len;
  const gchar *s;
  const gchar *end;
  const gchar *last_dot;
  int dot_count;
  gboolean last_element;

  g_return_val_if_fail (string != NULL, FALSE);

  len = strlen (string);
  if (G_UNLIKELY (len == 0))
    return FALSE;

  if (G_UNLIKELY (len > 255))
    return FALSE;

  end = string + len;

  last_dot = find_last_char (string, len, '.');
  last_element = FALSE;

  s = string;
  if (G_UNLIKELY (*s == '.'))
    return FALSE;

  if (G_UNLIKELY (!is_valid_initial_name_character (*s, last_element)))
    return FALSE;

  s += 1;
  dot_count = 0;
  while (s != end)
    {
      if (*s == '.')
        {
          if (s == last_dot)
            last_element = TRUE;
          s += 1;
          if (G_UNLIKELY (s == end))
            return FALSE;
          if (!is_valid_initial_name_character (*s, last_element))
            return FALSE;
          dot_count++;
        }
      else if (G_UNLIKELY (!is_valid_name_character (*s, last_element)))
        return FALSE;
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 2))
    return FALSE;

  return TRUE;
}

gboolean
xdp_app_info_flatpak_is_valid_sub_app_id (XdpAppInfo *app_info,
                                          const char *sub_app_id)
{
  const char *app_id = xdp_app_info_get_id (app_info);

  g_assert (app_id);

  if (!g_str_has_prefix (sub_app_id, app_id))
    return FALSE;

  if (sub_app_id[strlen (app_id)] != '.')
    return FALSE;

  return flatpak_is_valid_name (sub_app_id);
}

static char *
xdp_app_info_flatpak_remap_path (XdpAppInfo *app_info,
                                 const char *path)
{
  XdpAppInfoFlatpak *app_info_flatpak = XDP_APP_INFO_FLATPAK (app_info);
  const char *app_id = xdp_app_info_get_id (app_info);
  g_autofree char *app_path = NULL;
  g_autofree char *runtime_path = NULL;

  g_assert (app_info_flatpak);
  g_assert (app_id);

  app_path = g_key_file_get_string (app_info_flatpak->flatpak_info,
                                    FLATPAK_METADATA_GROUP_INSTANCE,
                                    FLATPAK_METADATA_KEY_APP_PATH,
                                    NULL);
  runtime_path = g_key_file_get_string (app_info_flatpak->flatpak_info,
                                        FLATPAK_METADATA_GROUP_INSTANCE,
                                        FLATPAK_METADATA_KEY_RUNTIME_PATH,
                                        NULL);

  /* For apps we translate /app and /usr to the installed locations.
     Also, we need to rewrite to drop the /newroot prefix added by
     bubblewrap for other files to work.  See
     https://github.com/projectatomic/bubblewrap/pull/172
     for a bit more information on the /newroot issue.
  */

  if (g_str_has_prefix (path, "/newroot/"))
    path = path + strlen ("/newroot");

  if (app_path != NULL && g_str_has_prefix (path, "/app/"))
    return g_build_filename (app_path, path + strlen ("/app/"), NULL);
  else if (runtime_path != NULL && g_str_has_prefix (path, "/usr/"))
    return g_build_filename (runtime_path, path + strlen ("/usr/"), NULL);
  else if (g_str_has_prefix (path, "/run/host/usr/"))
    return g_build_filename ("/usr", path + strlen ("/run/host/usr/"), NULL);
  else if (g_str_has_prefix (path, "/run/host/etc/"))
    return g_build_filename ("/etc", path + strlen ("/run/host/etc/"), NULL);
  else if (g_str_has_prefix (path, "/run/flatpak/app/"))
    return g_build_filename (g_get_user_runtime_dir (), "app",
                             path + strlen ("/run/flatpak/app/"), NULL);
  else if (g_str_has_prefix (path, "/run/flatpak/doc/"))
    return g_build_filename (g_get_user_runtime_dir (), "doc",
                             path + strlen ("/run/flatpak/doc/"), NULL);
  else if (g_str_has_prefix (path, "/var/config/"))
    return g_build_filename (g_get_home_dir (), ".var", "app",
                             app_id, "config",
                             path + strlen ("/var/config/"), NULL);
  else if (g_str_has_prefix (path, "/var/data/"))
    return g_build_filename (g_get_home_dir (), ".var", "app",
                             app_id, "data",
                             path + strlen ("/var/data/"), NULL);

  return g_strdup (path);
}

static char **
rewrite_commandline (XdpAppInfoFlatpak  *app_info,
                     const char * const *commandline,
                     gboolean            quote_escape)
{
  const char *app_id = xdp_app_info_get_id (XDP_APP_INFO (app_info));
  g_autoptr(GPtrArray) args = NULL;

  args = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("run"));

  if (commandline && commandline[0])
    {
      int i;
      g_autofree char *quoted_command = NULL;

      quoted_command = xdp_maybe_quote (commandline[0], quote_escape);

      g_ptr_array_add (args, g_strdup_printf ("--command=%s", quoted_command));

      /* Always quote the app ID if quote_escape is enabled to make
       * rewriting the file simpler in case the app is renamed.
       */
      if (quote_escape)
        g_ptr_array_add (args, g_shell_quote (app_id));
      else
        g_ptr_array_add (args, g_strdup (app_id));

      for (i = 1; commandline[i]; i++)
        g_ptr_array_add (args, xdp_maybe_quote (commandline[i], quote_escape));
    }
  else if (quote_escape)
    {
      g_ptr_array_add (args, g_shell_quote (app_id));
    }
  else
    {
      g_ptr_array_add (args, g_strdup (app_id));
    }

  g_ptr_array_add (args, NULL);
  return (char **)g_ptr_array_free (g_steal_pointer (&args), FALSE);
}

static char *
get_tryexec_path (XdpAppInfoFlatpak *app_info)
{
  const char *app_id = xdp_app_info_get_id (XDP_APP_INFO (app_info));
  g_autofree char *original_app_path = NULL;
  g_autofree char *app_path = NULL;
  char *path;
  g_autofree char *app_slash = NULL;
  char *app_slash_pointer;
  g_autofree char *tryexec_path = NULL;

  original_app_path = g_key_file_get_string (app_info->flatpak_info,
                                             FLATPAK_METADATA_GROUP_INSTANCE,
                                             FLATPAK_METADATA_KEY_ORIGINAL_APP_PATH,
                                             NULL);
  app_path = g_key_file_get_string (app_info->flatpak_info,
                                    FLATPAK_METADATA_GROUP_INSTANCE,
                                    FLATPAK_METADATA_KEY_APP_PATH,
                                    NULL);
  path = original_app_path ? original_app_path : app_path;

  if (path == NULL || *path == '\0')
    return NULL;

  app_slash = g_strconcat ("app/", app_id, NULL);

  app_slash_pointer = strstr (path, app_slash);
  if (app_slash_pointer == NULL)
    return NULL;

  /* Terminate path after the flatpak installation path such as
   * .local/share/flatpak/ */
  *app_slash_pointer = '\0';

  /* Find the path to the wrapper script exported by Flatpak, which can be
   * used in a desktop file's TryExec=
   */
  tryexec_path = g_strconcat (path, "exports/bin/", app_id, NULL);
  if (access (tryexec_path, X_OK) != 0)
    {
      g_debug ("Wrapper script unexpectedly not executable or nonexistent: %s",
               tryexec_path);
      return NULL;
    }

  return g_steal_pointer (&tryexec_path);
}

static gboolean
xdp_app_info_flatpak_validate_autostart (XdpAppInfo          *app_info,
                                         GKeyFile            *keyfile,
                                         const char * const  *autostart_exec,
                                         GCancellable        *cancellable,
                                         GError             **error)
{
  XdpAppInfoFlatpak *app_info_flatpak = XDP_APP_INFO_FLATPAK (app_info);
  const char *app_id = xdp_app_info_get_id (app_info);
  g_auto(GStrv) cmdv = NULL;
  g_autofree char *cmd = NULL;

  g_assert (app_info_flatpak);
  g_assert (app_id);

  cmdv = rewrite_commandline (app_info_flatpak,
                              autostart_exec,
                              FALSE /* don't quote escape */);
  cmd = g_strjoinv (" ", cmdv);

  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_EXEC,
                         cmd);

  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         "X-Flatpak",
                         app_id);

  return TRUE;
}

static const GPtrArray *
xdp_app_info_flaptak_get_usb_queries (XdpAppInfo *app_info)
{
  XdpAppInfoFlatpak *app_info_flatpak = XDP_APP_INFO_FLATPAK (app_info);

  if (!app_info_flatpak->queries)
    {
      g_autoptr(GPtrArray) usb_queries = NULL;

      usb_queries = g_ptr_array_new_with_free_func ((GDestroyNotify) xdp_usb_query_free);

      g_auto(GStrv) enumerable_devices = NULL;
      g_auto(GStrv) hidden_devices = NULL;

      enumerable_devices = g_key_file_get_string_list (app_info_flatpak->flatpak_info,
                                                       "USB Devices",
                                                       "enumerable-devices",
                                                       NULL, NULL);

      for (size_t i = 0; enumerable_devices && enumerable_devices[i] != NULL; i++)
        {
          g_autoptr(XdpUsbQuery) query =
            xdp_usb_query_from_string (XDP_USB_QUERY_TYPE_ENUMERABLE, enumerable_devices[i]);

          if (query)
            g_ptr_array_add (usb_queries, g_steal_pointer (&query));
        }

      hidden_devices = g_key_file_get_string_list (app_info_flatpak->flatpak_info,
                                                   "USB Devices",
                                                   "hidden-devices",
                                                   NULL, NULL);

      for (size_t i = 0; hidden_devices && hidden_devices[i] != NULL; i++)
        {
          g_autoptr(XdpUsbQuery) query =
            xdp_usb_query_from_string (XDP_USB_QUERY_TYPE_HIDDEN, hidden_devices[i]);

          if (query)
            g_ptr_array_add (usb_queries, g_steal_pointer (&query));
        }

      g_debug ("Found %d enumerable and %d hidden for app %s",
               enumerable_devices ? g_strv_length (enumerable_devices) : 0,
               hidden_devices ? g_strv_length (hidden_devices) : 0,
               xdp_app_info_get_id (app_info));
      app_info_flatpak->queries = g_steal_pointer (&usb_queries);
    }

  return app_info_flatpak->queries;
}

static gboolean
xdp_app_info_flatpak_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                                GKeyFile    *key_file,
                                                GError     **error)
{
  XdpAppInfoFlatpak *app_info_flatpak = XDP_APP_INFO_FLATPAK (app_info);
  const char *app_id = xdp_app_info_get_id (app_info);
  g_autofree char *exec = NULL;
  g_auto(GStrv) exec_strv = NULL;
  g_auto(GStrv) prefixed_exec_strv = NULL;
  g_autofree char *prefixed_exec = NULL;
  g_autofree char *tryexec_path = NULL;

  g_assert (app_info_flatpak);
  g_assert (app_id);

  exec = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "Exec", error);
  if (exec == NULL)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Desktop entry given to Install() has no Exec line");
      return FALSE;
    }

  if (!g_shell_parse_argv (exec, NULL, &exec_strv, error))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Desktop entry given to Install() has invalid Exec line");
      return FALSE;
    }

  /* Don't let the app give itself access to host files */
  if (g_strv_contains ((const char * const *)exec_strv, "--file-forwarding"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Desktop entry given to Install() must not use --file-forwarding");
      return FALSE;
    }

  prefixed_exec_strv = rewrite_commandline (app_info_flatpak,
                                            (const char * const *)exec_strv,
                                            TRUE /* quote escape */);
  prefixed_exec = g_strjoinv (" ", prefixed_exec_strv);
  g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "Exec", prefixed_exec);

  tryexec_path = get_tryexec_path (app_info_flatpak);
  if (tryexec_path != NULL)
    g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "TryExec", tryexec_path);

  /* Flatpak checks for this key */
  g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", app_id);
  /* Flatpak removes this one for security */
  g_key_file_remove_key (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-GNOME-Bugzilla-ExtraInfoScript", NULL);

  return TRUE;
}

static void
xdp_app_info_flatpak_dispose (GObject *object)
{
  XdpAppInfoFlatpak *app_info = XDP_APP_INFO_FLATPAK (object);

  g_clear_pointer (&app_info->flatpak_info, g_key_file_free);
  g_clear_pointer (&app_info->queries, g_ptr_array_unref);

  G_OBJECT_CLASS (xdp_app_info_flatpak_parent_class)->dispose (object);
}

static void
xdp_app_info_flatpak_class_init (XdpAppInfoFlatpakClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);

  object_class->dispose = xdp_app_info_flatpak_dispose;

  app_info_class->remap_path =
    xdp_app_info_flatpak_remap_path;
  app_info_class->get_usb_queries =
    xdp_app_info_flaptak_get_usb_queries;
  app_info_class->validate_autostart =
    xdp_app_info_flatpak_validate_autostart;
  app_info_class->validate_dynamic_launcher =
    xdp_app_info_flatpak_validate_dynamic_launcher;
  app_info_class->is_valid_sub_app_id =
    xdp_app_info_flatpak_is_valid_sub_app_id;
}

static void
xdp_app_info_flatpak_init (XdpAppInfoFlatpak *app_info_flatpak)
{
}

static int
open_pid_fd (int      proc_fd,
             pid_t    pid,
             GError **error)
{
  char buf[20] = {0, };
  int fd;

  snprintf (buf, sizeof(buf), "%u", (guint) pid);

  fd = openat (proc_fd, buf, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);

  if (fd == -1)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Could not to open '/proc/pid/%u': %s", (guint) pid,
                 g_strerror (errno));

  return fd;
}

static pid_t
get_bwrap_child_pid (JsonNode  *root,
                     GError   **error)
{
  JsonObject *cpo;
  pid_t pid;

  cpo = json_node_get_object (root);

  pid = json_object_get_int_member (cpo, "child-pid");
  if (pid == 0)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "child-pid missing");

  return pid;
}

static JsonNode *
load_bwrap_info (const char  *instance,
                 GError     **error)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonNode) root = NULL;
  g_autofree char *data = NULL;
  gsize len;
  g_autofree char *path = NULL;

  g_return_val_if_fail (instance != NULL, 0);

  path = g_build_filename (g_get_user_runtime_dir (),
                           ".flatpak",
                           instance,
                           "bwrapinfo.json",
                           NULL);

  if (!g_file_get_contents (path, &data, &len, error))
    return 0;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, len, error))
    {
      g_prefix_error (error, "Could not parse '%s': ", path);
      return 0;
    }

  root = json_node_ref (json_parser_get_root (parser));
  if (!root)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not parse '%s': empty file", path);
      return 0;
    }

  if (!JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not parse '%s': invalid structure", path);
      return 0;
    }

  return g_steal_pointer (&root);
}

static int
get_bwrap_pidfd (const char  *instance,
                 GError     **error)
{
  g_autoptr(JsonNode) root = NULL;
  DIR *proc;
  g_autofd int fd = -1;
  pid_t pid;

  if (instance == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not find instance-id in process's /.flatpak-info");
      return -1;
    }

  root = load_bwrap_info (instance, error);
  if (root == NULL)
    return -1;

  pid = get_bwrap_child_pid (root, error);
  if (pid == 0)
    return -1;

  proc = opendir ("/proc");
  if (proc == NULL)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not open '%s': %s", "/proc", g_strerror (errno));
      return -1;
    }

  fd = open_pid_fd (dirfd (proc), pid, error);
  closedir (proc);

  return g_steal_fd (&fd);
}

static int
open_flatpak_info (int      pid,
                   GError **error)
{
  g_autofree char *root_path = NULL;
  g_autofd int root_fd = -1;
  g_autofd int info_fd = -1;

  root_path = g_strdup_printf ("/proc/%u/root", pid);
  root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (root_fd == -1)
    {
      if (errno == EACCES)
        {
          struct statfs buf;

          /* Access to the root dir isn't allowed. This can happen if the root is on a fuse
           * filesystem, such as in a toolbox container. We will never have a fuse rootfs
           * in the flatpak case, so in that case its safe to ignore this and
           * continue to detect other types of apps.
           */
          if (statfs (root_path, &buf) == 0 &&
              buf.f_type == 0x65735546) /* FUSE_SUPER_MAGIC */
            {
              g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                           "Not a flatpak (fuse rootfs)");
              return -1;
            }
        }

      /* Otherwise, we should be able to open the root dir. Probably the app died and
         we're failing due to /proc/$pid not existing. In that case fail instead
         of treating this as privileged. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open %s", root_path);
      return -1;
    }

  info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (info_fd == -1)
    {
      if (errno == ENOENT)
        {
          /* No file => on the host, return success */
          g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                       "Not a flatpak (no .flatpak-info)");
          return -1;
        }

      /* Some weird error => failure */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return -1;
    }

  return g_steal_fd (&info_fd);
}

gboolean
xdp_is_flatpak (int        pid,
                gboolean  *is_flatpak,
                GError   **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autofd int info_fd = -1;

  info_fd = open_flatpak_info (pid, &local_error);
  if (info_fd == -1 && !g_error_matches (local_error, XDP_APP_INFO_ERROR,
                                         XDP_APP_INFO_ERROR_WRONG_APP_KIND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  *is_flatpak = info_fd != -1;
  return TRUE;
}

XdpAppInfo *
xdp_app_info_flatpak_new (int      pid,
                          int      pidfd,
                          GError **error)
{
  g_autoptr (XdpAppInfoFlatpak) app_info_flatpak = NULL;
  g_autofd int info_fd = -1;
  struct stat stat_buf;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  const char *group;
  g_autofree char *id = NULL;
  g_autofree char *instance = NULL;
  g_autofree char *desktop_id = NULL;
  g_autoptr(GAppInfo) gappinfo = NULL;
  g_auto(GStrv) shared = NULL;
  gboolean has_network;
  g_autofd int bwrap_pidfd = -1;

  info_fd = open_flatpak_info (pid, error);
  if (info_fd == -1)
    return NULL;

  if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    {
      /* Some weird fd => failure */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return NULL;
    }

  mapped = g_mapped_file_new_from_fd  (info_fd, FALSE, &local_error);
  if (mapped == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't map .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  metadata = g_key_file_new ();

  if (!g_key_file_load_from_data (metadata,
                                  g_mapped_file_get_contents (mapped),
                                  g_mapped_file_get_length (mapped),
                                  G_KEY_FILE_NONE, &local_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't load .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  group = FLATPAK_METADATA_GROUP_APPLICATION;
  if (g_key_file_has_group (metadata, FLATPAK_METADATA_GROUP_RUNTIME))
    group = FLATPAK_METADATA_GROUP_RUNTIME;

  id = g_key_file_get_string (metadata,
                              group,
                              FLATPAK_METADATA_KEY_NAME,
                              error);
  if (id == NULL || !xdp_is_valid_app_id (id))
    return NULL;

  instance = g_key_file_get_string (metadata,
                                    FLATPAK_METADATA_GROUP_INSTANCE,
                                    FLATPAK_METADATA_KEY_INSTANCE_ID,
                                    error);
  if (instance == NULL)
    return NULL;

  desktop_id = g_strconcat (id, ".desktop", NULL);
  gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  shared = g_key_file_get_string_list (metadata,
                                       FLATPAK_METADATA_GROUP_CONTEXT,
                                       FLATPAK_METADATA_KEY_SHARED,
                                       NULL, NULL);
  if (shared)
    {
      has_network = g_strv_contains ((const char * const *)shared,
                                     FLATPAK_METADATA_CONTEXT_SHARED_NETWORK);
    }
  else
    {
      has_network = FALSE;
    }

  /* flatpak has a xdg-dbus-proxy running which means we can't get the pidfd
   * of the connected process but we can get the pidfd of the bwrap instance
   * instead. This is okay because it has the same namespaces as the calling
   * process. */
  bwrap_pidfd = get_bwrap_pidfd (instance, error);
  if (bwrap_pidfd == -1)
    return NULL;

  /* TODO: we can use pidfd to make sure we didn't race for sure */

  app_info_flatpak = g_object_new (XDP_TYPE_APP_INFO_FLATPAK, NULL);
  xdp_app_info_initialize (XDP_APP_INFO (app_info_flatpak),
                           FLATPAK_ENGINE_ID, id, instance,
                           bwrap_pidfd, gappinfo,
                           TRUE, has_network, TRUE);
  app_info_flatpak->flatpak_info = g_steal_pointer (&metadata);

  return XDP_APP_INFO (g_steal_pointer (&app_info_flatpak));
}
