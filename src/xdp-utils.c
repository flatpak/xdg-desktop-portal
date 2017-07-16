/*
 * Copyright © 2014 Red Hat, Inc
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
 */

#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "xdp-utils.h"
#include "request.h"

G_LOCK_DEFINE (app_infos);
static GHashTable *app_infos;

static void
ensure_app_infos (void)
{
  if (app_infos == NULL)
    app_infos = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, (GDestroyNotify)g_key_file_unref);
}

/* Returns NULL on failure, keyfile with name "" if not sandboxed, and full app-info otherwise */
static GKeyFile *
parse_app_info_from_fileinfo (int pid, GError **error)
{
  g_autofree char *root_path = NULL;
  g_autofree char *path = NULL;
  g_autofree char *content = NULL;
  g_autofree char *app_id = NULL;
  int root_fd = -1;
  int info_fd = -1;
  struct stat stat_buf;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  g_autoptr(GKeyFile) metadata = NULL;

  root_path = g_strdup_printf ("/proc/%u/root", pid);
  root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (root_fd == -1)
    {
      /* Not able to open the root dir shouldn't happen. Probably the app died and
         we're failing due to /proc/$pid not existing. In that case fail instead
         of treating this as privileged. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open %s", root_path);
      return NULL;
    }

  metadata = g_key_file_new ();

  info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  close (root_fd);
  if (info_fd == -1)
    {
      if (errno == ENOENT)
        {
          /* No file => on the host */
          g_key_file_set_string (metadata, "Application", "name", "");
          return g_steal_pointer (&metadata);
        }

      /* Some weird error => failure */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return NULL;
    }

  if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    {
      /* Some weird fd => failure */
      close (info_fd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return NULL;
    }

  mapped = g_mapped_file_new_from_fd  (info_fd, FALSE, &local_error);
  if (mapped == NULL)
    {
      close (info_fd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't map .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  if (!g_key_file_load_from_data (metadata,
                                  g_mapped_file_get_contents (mapped),
                                  g_mapped_file_get_length (mapped),
                                  G_KEY_FILE_NONE, &local_error))
    {
      close (info_fd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't load .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  return g_steal_pointer (&metadata);
}

static char *
xdp_get_app_id_from_info (GKeyFile *app_info,
                          GError **error)
{
  const char *group = "Application";
  if (g_key_file_has_group (app_info, "Runtime"))
    group = "Runtime";

  return g_key_file_get_string (app_info, group, "name", error);
}

char *
xdp_get_app_id_from_pid (pid_t pid,
                         GError **error)
{
  g_autoptr(GKeyFile) app_info = NULL;

  app_info = parse_app_info_from_fileinfo (pid, error);
  if (app_info == NULL)
    return NULL;

  return xdp_get_app_id_from_info (app_info, error);
}

static GKeyFile *
lookup_cached_app_info_by_sender (const char *sender)
{
  GKeyFile *app_info = NULL;

  G_LOCK (app_infos);
  if (app_infos)
    {
      app_info = g_hash_table_lookup (app_infos, sender);
      if (app_info)
        g_key_file_ref (app_info);
    }
  G_UNLOCK (app_infos);

  return app_info;
}


static char *
xdp_connection_lookup_app_id_sync (GDBusConnection       *connection,
                                   const char            *sender,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  g_autoptr(GDBusMessage) msg = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(GKeyFile) app_info = NULL;
  char *app_id = NULL;
  GVariant *body;
  guint32 pid;

  app_info = lookup_cached_app_info_by_sender (sender);
  if (app_info)
    return xdp_get_app_id_from_info (app_info, error);

  msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "GetConnectionUnixProcessID");
  g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

  reply = g_dbus_connection_send_message_with_reply_sync (connection, msg,
                                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                          30000,
                                                          NULL,
                                                          cancellable,
                                                          error);
  if (reply == NULL)
    return NULL;

  if (g_dbus_message_get_message_type (reply) == G_DBUS_MESSAGE_TYPE_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer app id");
      return NULL;
    }

  body = g_dbus_message_get_body (reply);

  g_variant_get (body, "(u)", &pid);

  app_info = parse_app_info_from_fileinfo (pid, error);
  if (app_info == NULL)
    return NULL;

  app_id = xdp_get_app_id_from_info (app_info, error);
  if (app_id)
    {
      G_LOCK (app_infos);
      ensure_app_infos ();
      g_hash_table_insert (app_infos, g_strdup (sender), g_key_file_ref (app_info));
      G_UNLOCK (app_infos);
    }

  return app_id;
}

GKeyFile *
xdp_invocation_lookup_cached_app_info (GDBusMethodInvocation *invocation)
{
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  return lookup_cached_app_info_by_sender (sender);
}

char *
xdp_invocation_lookup_app_id_sync (GDBusMethodInvocation *invocation,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  return xdp_connection_lookup_app_id_sync (connection, sender, cancellable, error);
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(sss)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      G_LOCK (app_infos);
      if (app_infos)
        g_hash_table_remove (app_infos, name);
      G_UNLOCK (app_infos);

      close_requests_for_sender (name);
    }
}

void
xdp_connection_track_name_owners (GDBusConnection *connection)
{
  g_dbus_connection_signal_subscribe (connection,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);
}

void
xdp_filter_options (GVariant *options,
                    GVariantBuilder *filtered,
                    XdpOptionKey *supported_options,
                    int n_supported_options)
{
  GVariant *value;
  int i;

  for (i = 0; i < n_supported_options; i++)
    {
      value = g_variant_lookup_value (options,
                                      supported_options[i].key,
                                      supported_options[i].type);
      if (value)
         g_variant_builder_add (filtered, "{sv}", supported_options[i].key, value);
    }
}

static const GDBusErrorEntry xdg_desktop_portal_error_entries[] = {
  { XDG_DESKTOP_PORTAL_ERROR_FAILED,           "org.freedesktop.portal.Error.Failed" },
  { XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT, "org.freedesktop.portal.Error.InvalidArgument" },
  { XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,        "org.freedesktop.portal.Error.NotFound" },
  { XDG_DESKTOP_PORTAL_ERROR_EXISTS,           "org.freedesktop.portal.Error.Exists" },
  { XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,      "org.freedesktop.portal.Error.NotAllowed" },
  { XDG_DESKTOP_PORTAL_ERROR_CANCELLED,        "org.freedesktop.portal.Error.Cancelled" },
  { XDG_DESKTOP_PORTAL_ERROR_WINDOW_DESTROYED, "org.freedesktop.portal.Error.WindowDestroyed" }
};

GQuark
xdg_desktop_portal_error_quark (void)
{
  static volatile gsize quark_volatile = 0;

  g_dbus_error_register_error_domain ("xdg-desktop-portal-error-quark",
                                      &quark_volatile,
                                      xdg_desktop_portal_error_entries,
                                      G_N_ELEMENTS (xdg_desktop_portal_error_entries));
  return (GQuark) quark_volatile;
}

char *
xdp_get_path_for_fd (GKeyFile *app_info,
                     int fd)
{
  g_autofree char *proc_path = NULL;
  int fd_flags;
  char path_buffer[PATH_MAX + 1];
  g_autofree char *rewritten_path = NULL;
  char *path;
  ssize_t symlink_size = 0;
  struct stat st_buf;
  struct stat real_st_buf;

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  if (fd == -1 ||
      (fd_flags = fcntl (fd, F_GETFL)) == 0 ||
      ((fd_flags & O_PATH) != O_PATH) ||
      ((fd_flags & O_NOFOLLOW) == O_NOFOLLOW) ||
      fstat (fd, &st_buf) < 0 ||
      (st_buf.st_mode & S_IFMT) != S_IFREG ||
      (symlink_size = readlink (proc_path, path_buffer, PATH_MAX)) < 0)
    {
      return NULL;
    }

  path_buffer[symlink_size] = 0;
  path = path_buffer;

  if (app_info != NULL)
    {
      if (g_str_has_prefix (path, "/newroot/usr/"))
        {
          g_autofree char *usr_root = NULL;

          usr_root = g_key_file_get_string (app_info, "Instance", "runtime-path", NULL);
          if (usr_root)
            {
              rewritten_path = g_build_filename (usr_root, path + strlen ("/newroot/usr/"), NULL);
              path = rewritten_path;
            }
        }
      else if (g_str_has_prefix (path, "/newroot/app/"))
        {
          g_autofree char *app_root = NULL;

          app_root = g_key_file_get_string (app_info, "Instance", "app-path", NULL);
          if (app_root)
            {
              rewritten_path = g_build_filename (app_root, path + strlen ("/newroot/app/"), NULL);
              path = rewritten_path;
            }
        }
      else if (g_str_has_prefix (path, "/newroot/"))
        path = path + strlen ("/newroot");

      /* Verify that this is the same file as the app opened */
      if (stat (path, &real_st_buf) < 0 ||
          st_buf.st_dev != real_st_buf.st_dev ||
          st_buf.st_ino != real_st_buf.st_ino)
        {
          /* Different files on the inside and the outside, reject the request */
          return NULL;
        }
    }

  return g_strdup (path);
}
