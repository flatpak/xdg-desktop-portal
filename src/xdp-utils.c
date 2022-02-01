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

#include <json-glib/json-glib.h>

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <sys/vfs.h>

#include <gio/gdesktopappinfo.h>

#include "xdp-utils.h"

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

G_LOCK_DEFINE (app_infos);
static GHashTable *app_info_by_unique_name;

/* Based on g_mkstemp from glib */

gint
xdp_mkstempat (int    dir_fd,
               gchar *tmpl,
               int    flags,
               int    mode)
{
  char *XXXXXX;
  int count, fd;
  static const char letters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static const int NLETTERS = sizeof (letters) - 1;
  glong value;
  GTimeVal tv;
  static int counter = 0;

  g_return_val_if_fail (tmpl != NULL, -1);

  /* find the last occurrence of "XXXXXX" */
  XXXXXX = g_strrstr (tmpl, "XXXXXX");

  if (!XXXXXX || strncmp (XXXXXX, "XXXXXX", 6))
    {
      errno = EINVAL;
      return -1;
    }

  /* Get some more or less random data.  */
  g_get_current_time (&tv);
  value = (tv.tv_usec ^ tv.tv_sec) + counter++;

  for (count = 0; count < 100; value += 7777, ++count)
    {
      glong v = value;

      /* Fill in the random bits.  */
      XXXXXX[0] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[1] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[2] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[3] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[4] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[5] = letters[v % NLETTERS];

      fd = openat (dir_fd, tmpl, flags | O_CREAT | O_EXCL, mode);

      if (fd >= 0)
        return fd;
      else if (errno != EEXIST)
        /* Any other error will apply also to other names we might
         *  try, and there are 2^32 or so of them, so give up now.
         */
        return -1;
    }

  /* We got out of the loop because we ran out of combinations to try.  */
  errno = EEXIST;
  return -1;
}

typedef enum
{
  XDP_APP_INFO_KIND_HOST = 0,
  XDP_APP_INFO_KIND_FLATPAK = 1,
  XDP_APP_INFO_KIND_SNAP    = 2,
} XdpAppInfoKind;

struct _XdpAppInfo {
  volatile gint ref_count;
  char *id;
  XdpAppInfoKind kind;

  union
    {
      struct
        {
          GKeyFile *keyfile;
	   /* pid namespace mapping */
          GMutex pidns_lock;
          ino_t   pidns_id;
        } flatpak;
      struct
        {
          GKeyFile *keyfile;
        } snap;
    } u;
};

static XdpAppInfo *
xdp_app_info_new (XdpAppInfoKind kind)
{
  XdpAppInfo *app_info = g_new0 (XdpAppInfo, 1);
  app_info->ref_count = 1;
  app_info->kind = kind;
  return app_info;
}

static XdpAppInfo *
xdp_app_info_new_host (void)
{
  XdpAppInfo *app_info = xdp_app_info_new (XDP_APP_INFO_KIND_HOST);
  app_info->id = g_strdup ("");
  return app_info;
}

static void
xdp_app_info_free (XdpAppInfo *app_info)
{
  g_free (app_info->id);

  switch (app_info->kind)
    {
    case XDP_APP_INFO_KIND_FLATPAK:
      g_clear_pointer (&app_info->u.flatpak.keyfile, g_key_file_free);
      break;

    case XDP_APP_INFO_KIND_SNAP:
      g_clear_pointer (&app_info->u.snap.keyfile, g_key_file_free);
      break;

    case XDP_APP_INFO_KIND_HOST:
    default:
      break;
    }

  g_free (app_info);
}

XdpAppInfo *
xdp_app_info_ref (XdpAppInfo *app_info)
{
  g_return_val_if_fail (app_info != NULL, NULL);

  g_atomic_int_inc (&app_info->ref_count);
  return app_info;
}

void
xdp_app_info_unref (XdpAppInfo *app_info)
{
  g_return_if_fail (app_info != NULL);

  if (g_atomic_int_dec_and_test (&app_info->ref_count))
    xdp_app_info_free (app_info);
}

const char *
xdp_app_info_get_id (XdpAppInfo *app_info)
{
  g_return_val_if_fail (app_info != NULL, NULL);

  return app_info->id;
}

GAppInfo *
xdp_app_info_load_app_info (XdpAppInfo *app_info)
{
  g_autofree char *desktop_id = NULL;

  g_return_val_if_fail (app_info != NULL, NULL);

  switch (app_info->kind)
    {
    case XDP_APP_INFO_KIND_FLATPAK:
      desktop_id = g_strconcat (app_info->id, ".desktop", NULL);
      break;

    case XDP_APP_INFO_KIND_SNAP:
      desktop_id = g_key_file_get_string (app_info->u.snap.keyfile,
                                          SNAP_METADATA_GROUP_INFO,
                                          SNAP_METADATA_KEY_DESKTOP_FILE,
                                          NULL);
      break;

    case XDP_APP_INFO_KIND_HOST:
    default:
      desktop_id = NULL;
      break;
    }

  if (desktop_id == NULL)
    return NULL;

  return G_APP_INFO (g_desktop_app_info_new (desktop_id));
}

char **
xdp_app_info_rewrite_commandline (XdpAppInfo *app_info,
                                  const char * const *commandline)
{
  g_return_val_if_fail (app_info != NULL, NULL);

  if (app_info->kind == XDP_APP_INFO_KIND_HOST)
    {
      return g_strdupv ((char **)commandline);
    }
  else if (app_info->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
      g_autoptr(GPtrArray) args = NULL;

      args = g_ptr_array_new_with_free_func (g_free);

      g_ptr_array_add (args, g_strdup ("flatpak"));
      g_ptr_array_add (args, g_strdup ("run"));
      if (commandline && commandline[0])
        {
          int i;

          g_ptr_array_add (args, g_strdup_printf ("--command=%s", commandline[0]));
          g_ptr_array_add (args, g_strdup (app_info->id));
          for (i = 1; commandline[i]; i++)
            g_ptr_array_add (args, g_strdup (commandline[i]));
        }
      else
        g_ptr_array_add (args, g_strdup (app_info->id));
      g_ptr_array_add (args, NULL);

      return (char **)g_ptr_array_free (g_steal_pointer (&args), FALSE);
    }
  else
    return NULL;
}

char *
xdp_app_info_get_instance (XdpAppInfo *app_info)
{
  g_return_val_if_fail (app_info != NULL, NULL);

  if (app_info->kind != XDP_APP_INFO_KIND_FLATPAK)
    return NULL;

  return g_key_file_get_string (app_info->u.flatpak.keyfile,
                                FLATPAK_METADATA_GROUP_INSTANCE,
                                FLATPAK_METADATA_KEY_INSTANCE_ID,
                                NULL);
}

gboolean
xdp_app_info_is_host (XdpAppInfo *app_info)
{
  g_return_val_if_fail (app_info != NULL, FALSE);

  return app_info->kind == XDP_APP_INFO_KIND_HOST;
}

gboolean
xdp_app_info_supports_opath (XdpAppInfo  *app_info)
{
  return
    app_info->kind == XDP_APP_INFO_KIND_FLATPAK ||
    app_info->kind == XDP_APP_INFO_KIND_HOST;
}

char *
xdp_app_info_remap_path (XdpAppInfo *app_info,
                         const char *path)
{
  if (app_info->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
      g_autofree char *app_path = g_key_file_get_string (app_info->u.flatpak.keyfile,
                                                         FLATPAK_METADATA_GROUP_INSTANCE,
                                                         FLATPAK_METADATA_KEY_APP_PATH, NULL);
      g_autofree char *runtime_path = g_key_file_get_string (app_info->u.flatpak.keyfile,
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
                                 app_info->id, "config",
                                 path + strlen ("/var/config/"), NULL);
      else if (g_str_has_prefix (path, "/var/data/"))
        return g_build_filename (g_get_home_dir (), ".var", "app",
                                 app_info->id, "data",
                                 path + strlen ("/var/data/"), NULL);
    }

  return g_strdup (path);
}

gboolean
xdp_app_info_has_network (XdpAppInfo *app_info)
{
  gboolean has_network;

  switch (app_info->kind)
    {
    case XDP_APP_INFO_KIND_FLATPAK:
      {
        g_auto(GStrv) shared = g_key_file_get_string_list (app_info->u.flatpak.keyfile,
                                                           "Context", "shared",
                                                           NULL, NULL);
        if (shared)
          has_network = g_strv_contains ((const char * const *)shared, "network");
        else
          has_network = FALSE;
      }
      break;

    case XDP_APP_INFO_KIND_SNAP:
      has_network = g_key_file_get_boolean (app_info->u.snap.keyfile,
                                            SNAP_METADATA_GROUP_INFO,
                                            SNAP_METADATA_KEY_NETWORK, NULL);
      break;

    case XDP_APP_INFO_KIND_HOST:
    default:
      has_network = TRUE;
      break;
    }

  return has_network;
}

static void
ensure_app_info_by_unique_name (void)
{
  if (app_info_by_unique_name == NULL)
    app_info_by_unique_name = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free,
                                                     (GDestroyNotify)xdp_app_info_unref);
}

/* Returns NULL with error set on failure, NULL with no error set if not a flatpak, and app-info otherwise */
static XdpAppInfo *
parse_app_info_from_flatpak_info (int pid, GError **error)
{
  g_autofree char *root_path = NULL;
  int root_fd = -1;
  int info_fd = -1;
  struct stat stat_buf;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  const char *group;
  g_autofree char *id = NULL;

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
            return NULL;
        }

      /* Otherwise, we should be able to open the root dir. Probably the app died and
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
          /* No file => on the host, return NULL with no error */
          return NULL;
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

  group = "Application";
  if (g_key_file_has_group (metadata, "Runtime"))
    group = "Runtime";

  id = g_key_file_get_string (metadata, group, "name", error);
  if (id == NULL)
    {
      close (info_fd);
      return NULL;
    }

  close (info_fd);

  app_info = xdp_app_info_new (XDP_APP_INFO_KIND_FLATPAK);
  app_info->id = g_steal_pointer (&id);
  app_info->u.flatpak.keyfile = g_steal_pointer (&metadata);

  return g_steal_pointer (&app_info);
}

int
_xdp_parse_cgroup_file (FILE *f, gboolean *is_snap)
{
  ssize_t n;
  g_autofree char *id = NULL;
  g_autofree char *controller = NULL;
  g_autofree char *cgroup = NULL;
  size_t id_len = 0, controller_len = 0, cgroup_len = 0;

  g_return_val_if_fail(f != NULL, -1);
  g_return_val_if_fail(is_snap != NULL, -1);

  *is_snap = FALSE;
  do
    {
      n = getdelim (&id, &id_len, ':', f);
      if (n == -1) break;
      n = getdelim (&controller, &controller_len, ':', f);
      if (n == -1) break;
      n = getdelim (&cgroup, &cgroup_len, '\n', f);
      if (n == -1) break;

      /* Only consider the freezer, systemd group or unified cgroup
       * hierarchies */
      if ((!strcmp (controller, "freezer:") != 0 ||
           !strcmp (controller, "name=systemd:") != 0 ||
           !strcmp (controller, ":") != 0) &&
          strstr (cgroup, "/snap.") != NULL)
        {
          *is_snap = TRUE;
          break;
        }
    }
  while (n >= 0);

  if (n < 0 && !feof(f)) return -1;

  return 0;
}

static gboolean
pid_is_snap (pid_t pid, GError **error)
{
  g_autofree char *cgroup_path = NULL;;
  int fd;
  FILE *f = NULL;
  gboolean is_snap = FALSE;
  int err = 0;

  g_return_val_if_fail(pid > 0, FALSE);

  cgroup_path = g_strdup_printf ("/proc/%u/cgroup", (guint) pid);
  fd = open (cgroup_path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd == -1)
    {
      err = errno;
      goto end;
    }

  f = fdopen (fd, "r");
  if (f == NULL)
    {
      err = errno;
      goto end;
    }

  fd = -1; /* fd is now owned by f */

  if (_xdp_parse_cgroup_file (f, &is_snap) == -1)
    err = errno;

  fclose (f);

end:
  /* Silence ENOENT, treating it as "not a snap" */
  if (err != 0 && err != ENOENT)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                   "Could not parse cgroup info for pid %u: %s", (guint) pid,
                   g_strerror (err));
    }
  return is_snap;
}

/* Returns NULL with error set on failure, NULL with no error set if not a snap, and app-info otherwise */
static XdpAppInfo *
parse_app_info_from_snap (pid_t pid, GError **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autofree char *pid_str = NULL;
  const char *argv[] = { "snap", "routine", "portal-info", NULL, NULL };
  g_autofree char *output = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autofree char *snap_name = NULL;

  /* Check the process's cgroup membership to fail quickly for non-snaps */
  if (!pid_is_snap (pid, error)) return NULL;

  pid_str = g_strdup_printf ("%u", (guint) pid);
  argv[3] = pid_str;
  if (!xdp_spawnv (NULL, &output, 0, error, argv))
    {
      return NULL;
    }

  metadata = g_key_file_new ();
  if (!g_key_file_load_from_data (metadata, output, -1, G_KEY_FILE_NONE, &local_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't read snap info for pid %u: %s", pid, local_error->message);
      return NULL;
    }

  snap_name = g_key_file_get_string (metadata, SNAP_METADATA_GROUP_INFO,
                                     SNAP_METADATA_KEY_INSTANCE_NAME, error);
  if (snap_name == NULL)
    {
      return NULL;
    }

  app_info = xdp_app_info_new (XDP_APP_INFO_KIND_SNAP);
  app_info->id = g_strconcat ("snap.", snap_name, NULL);
  app_info->u.snap.keyfile = g_steal_pointer (&metadata);

  return g_steal_pointer (&app_info);
}


XdpAppInfo *
xdp_get_app_info_from_pid (pid_t pid,
                           GError **error)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) local_error = NULL;

  app_info = parse_app_info_from_flatpak_info (pid, &local_error);
  if (app_info == NULL && local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  if (app_info == NULL)
    {
      app_info = parse_app_info_from_snap (pid, &local_error);
      if (app_info == NULL && local_error)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }
    }

  if (app_info == NULL)
    app_info = xdp_app_info_new_host ();

  return g_steal_pointer (&app_info);
}

static XdpAppInfo *
lookup_cached_app_info_by_sender (const char *sender)
{
  XdpAppInfo *app_info = NULL;

  G_LOCK (app_infos);
  if (app_info_by_unique_name)
    {
      app_info = g_hash_table_lookup (app_info_by_unique_name, sender);
      if (app_info)
        xdp_app_info_ref (app_info);
    }
  G_UNLOCK (app_infos);

  return app_info;
}

static XdpAppInfo *
xdp_connection_lookup_app_info_sync (GDBusConnection       *connection,
                                     const char            *sender,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  g_autoptr(GDBusMessage) msg = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  GVariant *body;
  guint32 pid = 0;

  app_info = lookup_cached_app_info_by_sender (sender);
  if (app_info)
    return g_steal_pointer (&app_info);

  msg = g_dbus_message_new_method_call (DBUS_NAME_DBUS,
                                        DBUS_PATH_DBUS,
                                        DBUS_INTERFACE_DBUS,
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

  app_info = xdp_get_app_info_from_pid (pid, error);
  if (app_info == NULL)
    return NULL;

  G_LOCK (app_infos);
  ensure_app_info_by_unique_name ();
  g_hash_table_insert (app_info_by_unique_name, g_strdup (sender),
                       xdp_app_info_ref (app_info));
  G_UNLOCK (app_infos);

  return g_steal_pointer (&app_info);
}

XdpAppInfo *
xdp_invocation_lookup_app_info_sync (GDBusMethodInvocation *invocation,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  return xdp_connection_lookup_app_info_sync (connection, sender, cancellable, error);
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
  XdpPeerDiedCallback peer_died_cb = user_data;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      G_LOCK (app_infos);
      if (app_info_by_unique_name)
        g_hash_table_remove (app_info_by_unique_name, name);
      G_UNLOCK (app_infos);

      if (peer_died_cb)
        peer_died_cb (name);
    }
}

void
xdp_connection_track_name_owners (GDBusConnection *connection,
                                  XdpPeerDiedCallback peer_died_cb)
{
  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      peer_died_cb, NULL);
}

gboolean
xdp_filter_options (GVariant *options,
                    GVariantBuilder *filtered,
                    XdpOptionKey *supported_options,
                    int n_supported_options,
                    GError **error)
{
  int i;
  gboolean ret = TRUE;

  for (i = 0; i < n_supported_options; i++)
    {
      g_autoptr(GVariant) value = NULL;

      value = g_variant_lookup_value (options,
                                      supported_options[i].key,
                                      supported_options[i].type);
      if (!value)
        {
          value = g_variant_lookup_value (options, supported_options[i].key, NULL);
          if (value)
            {
              if (*error == NULL)
                g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                             "Expected type '%s' for option '%s', got '%s'",
                             g_variant_type_peek_string (supported_options[i].type),
                             supported_options[i].key,
                             g_variant_type_peek_string (g_variant_get_type (value)));
              ret = FALSE;
            }

          continue;
        }
         
      if (supported_options[i].validate)
        {
          g_autoptr(GError) local_error = NULL;

          if (!supported_options[i].validate (supported_options[i].key, value, options, &local_error))
            {
              if (ret)
                {
                  ret = FALSE;
                  if (error && *error == NULL)
                    {
                      g_propagate_error (error, local_error);
                      local_error = NULL;
                    }
                }

              continue;
            }
        }

      g_variant_builder_add (filtered, "{sv}", supported_options[i].key, g_steal_pointer (&value));
    }

  return ret;
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

static char *
verify_proc_self_fd (XdpAppInfo *app_info,
                     const char *proc_path,
                     GError **error)
{
  char path_buffer[PATH_MAX + 1];
  ssize_t symlink_size;
  int saved_errno;

  symlink_size = readlink (proc_path, path_buffer, PATH_MAX);
  if (symlink_size < 0)
    {
      saved_errno = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "readlink %s: %s", proc_path, g_strerror (saved_errno));
      return NULL;
    }

  path_buffer[symlink_size] = 0;

  /* All normal paths start with /, but some weird things
     don't, such as socket:[27345] or anon_inode:[eventfd].
     We don't support any of these */
  if (path_buffer[0] != '/')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                   "Not a regular file or directory: %s", path_buffer);
      return NULL;
    }

  /* File descriptors to actually deleted files have " (deleted)"
     appended to them. This also happens to some fake fd types
     like shmem which are "/<name> (deleted)". All such
     files are considered invalid. Unfortunatelly this also
     matches files with filenames that actually end in " (deleted)",
     but there is not much to do about this. */
  if (g_str_has_suffix (path_buffer, " (deleted)"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                   "Cannot share deleted file: %s", path_buffer);
      return NULL;
    }

  /* remap from sandbox to host if needed */
  return xdp_app_info_remap_path (app_info, path_buffer);
}

static char *documents_mountpoint = NULL;

void
xdp_set_documents_mountpoint (const char *path)
{
  g_clear_pointer (&documents_mountpoint, g_free);
  documents_mountpoint = g_strdup (path);
}

/* alternate_document_path converts a file path  */
char *
xdp_get_alternate_document_path (const char *path, const char *app_id)
{
  int len;

  /* If we don't know where the document portal is mounted, then there
   * is no alternate path */
  if (documents_mountpoint == NULL)
    return NULL;

  /* If the path is not within the document portal, then there is no
   * alternative path */
  len = strlen (documents_mountpoint);
  if (!g_str_has_prefix (path, documents_mountpoint) || path[len] != '/')
    return NULL;

  return g_strconcat (documents_mountpoint, "/by-app/", app_id, &path[len], NULL);
}

static gboolean
check_same_file (const char *path,
                 struct stat *expected_st_buf,
                 GError **error)
{
  struct stat real_st_buf;
  int saved_errno;

  if (stat (path, &real_st_buf) < 0)
    {
      saved_errno = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "stat %s: %s", path, g_strerror (saved_errno));
      return FALSE;
    }

  if (expected_st_buf->st_dev != real_st_buf.st_dev ||
      expected_st_buf->st_ino != real_st_buf.st_ino)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "\"%s\" identity (%ju,%ju) does not match expected (%ju,%ju)",
                   path,
                   (uintmax_t) expected_st_buf->st_dev,
                   (uintmax_t) expected_st_buf->st_ino,
                   (uintmax_t) real_st_buf.st_dev,
                   (uintmax_t) real_st_buf.st_ino);
      return FALSE;
    }

  return TRUE;
}

char *
xdp_app_info_get_path_for_fd (XdpAppInfo *app_info,
                              int fd,
                              int require_st_mode,
                              struct stat *st_buf,
                              gboolean *writable_out,
                              GError **error)
{
  g_autofree char *proc_path = NULL;
  int fd_flags;
  struct stat st_buf_store;
  gboolean writable = FALSE;
  g_autofree char *path = NULL;
  int saved_errno;

  if (st_buf == NULL)
    st_buf = &st_buf_store;

  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid file descriptor");
      return NULL;
    }

  /* Must be able to get fd flags */
  fd_flags = fcntl (fd, F_GETFL);
  if (fd_flags == -1)
    {
      saved_errno = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Cannot get file descriptor flags (fcntl F_GETFL: %s)",
                   g_strerror (saved_errno));
      return NULL;
    }

  /* Must be able to fstat */
  if (fstat (fd, st_buf) < 0)
    {
      saved_errno = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Cannot get file information (fstat: %s)",
                   g_strerror (saved_errno));
      return NULL;
    }

  /* Verify mode */
  if (require_st_mode != 0 &&
      (st_buf->st_mode & S_IFMT) != require_st_mode)
    {
      switch (require_st_mode)
        {
          case S_IFDIR:
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                         "File type 0o%o is not a directory",
                         (st_buf->st_mode & S_IFMT));
            return NULL;

          case S_IFREG:
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                         "File type 0o%o is not a regular file",
                         (st_buf->st_mode & S_IFMT));
            return NULL;

          default:
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "File type 0o%o does not match expected 0o%o",
                         (st_buf->st_mode & S_IFMT), require_st_mode);
            return NULL;
        }
    }

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  /* Must be able to read valid path from /proc/self/fd */
  /* This is an absolute and (at least at open time) symlink-expanded path */
  path = verify_proc_self_fd (app_info, proc_path, error);
  if (path == NULL)
    return NULL;

  if ((fd_flags & O_PATH) == O_PATH)
    {
      int read_access_mode;

      /* Earlier versions of the portal supported only O_PATH fds, as
       * these are safer to handle on the portal side. But we now
       * prefer regular FDs because these ensure that the sandbox
       * actually has full access to the file in its security context.
       *
       * However, we still support O_PATH fds when possible because
       * existing code uses it.
       *
       * See issues #167 for details.
       */

      /* Must not be O_NOFOLLOW (because we want the target file) */
      if ((fd_flags & O_NOFOLLOW) == O_NOFOLLOW)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "O_PATH fd was opened O_NOFOLLOW");
          return NULL;
        }

      if (!xdp_app_info_supports_opath (app_info))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "App \"%s\" of type %d does not support O_PATH fd passing",
                       app_info->id, app_info->kind);
          return NULL;
        }

      read_access_mode = R_OK;
      if (S_ISDIR (st_buf->st_mode))
        read_access_mode |= X_OK;

      /* Must be able to access the path via the sandbox supplied O_PATH fd,
         which applies the sandbox side mount options (like readonly). */
      if (access (proc_path, read_access_mode) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "\"%s\" not available for read access via \"%s\"",
                       path, proc_path);
          return NULL;
        }

      if (xdp_app_info_is_host (app_info) || access (proc_path, W_OK) == 0)
        writable = TRUE;
    }
  else /* Regular file with no O_PATH */
    {
      int accmode = fd_flags & O_ACCMODE;

      /* Note that this only gives valid results for writable for regular files,
         as there is no way to get a writable fd for a directory. */

      /* Don't allow WRONLY (or weird) open modes */
      if (accmode != O_RDONLY &&
          accmode != O_RDWR)
        return NULL;

      if (xdp_app_info_is_host (app_info) || accmode == O_RDWR)
        writable = TRUE;
    }

  /* Verify that this is the same file as the app opened */
  if (!check_same_file (path, st_buf, error))
    {
      /* If the path is provided by the document portal, the inode
         number will not match, due to only a subtree being mounted in
         the sandbox.  So we check to see if the equivalent path
         within that subtree matches our file descriptor.

         If the alternate path doesn't match either, then we treat it
         as a failure.
      */
      g_autofree char *alt_path = NULL;
      alt_path = xdp_get_alternate_document_path (path, xdp_app_info_get_id (app_info));

      if (alt_path == NULL)
        return NULL;

      g_clear_error (error);

      if (!check_same_file (alt_path, st_buf, error))
        return NULL;
    }

  if (writable_out)
    *writable_out = writable;

  return g_steal_pointer (&path);
}

static gboolean
is_valid_name_character (gint c, gboolean allow_dash)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c >= '0' && c <= '9') ||
    (c == '_') || (allow_dash && c == '-');
}

/* This is the same as flatpak apps, except we also allow
   names to start with digits, and two-element names so that ids of the form
   snap.$snapname is allowed for all snap names. */
gboolean
xdp_is_valid_app_id (const char *string)
{
  guint len;
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

  last_dot = strrchr (string, '.');
  last_element = FALSE;

  s = string;
  if (G_UNLIKELY (*s == '.'))
    return FALSE; /* Name can't start with a period */

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
          dot_count++;
        }

      if (G_UNLIKELY (!is_valid_name_character (*s, last_element)))
        return FALSE;
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 1))
    return FALSE;

  return TRUE;
}


static gboolean
needs_quoting (const char *arg)
{
  while (*arg != 0)
    {
      char c = *arg;
      if (!g_ascii_isalnum (c) &&
          !(c == '-' || c == '/' || c == '~' ||
            c == ':' || c == '.' || c == '_' ||
            c == '='))
        return TRUE;
      arg++;
    }
  return FALSE;
}

char *
xdp_quote_argv (const char *argv[])
{
  GString *res = g_string_new ("");
  int i;

  for (i = 0; argv[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append_c (res, ' ');

      if (needs_quoting (argv[i]))
        {
          g_autofree char *quoted = g_shell_quote (argv[i]);
          g_string_append (res, quoted);
        }
      else
        g_string_append (res, argv[i]);
    }

  return g_string_free (res, FALSE);
}

typedef struct
{
  GError    *error;
  GError    *splice_error;
  GMainLoop *loop;
  int        refs;
} SpawnData;

static void
spawn_data_exit (SpawnData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
spawn_output_spliced_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  SpawnData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  spawn_data_exit (data);
}

static void
spawn_exit_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  SpawnData *data = user_data;

  g_subprocess_wait_check_finish (G_SUBPROCESS (obj), result, &data->error);
  spawn_data_exit (data);
}

gboolean
xdp_spawn (GFile       *dir,
           char       **output,
           GSubprocessFlags flags,
           GError     **error,
           const gchar *argv0,
           va_list      ap)
{
  GPtrArray *args;
  const gchar *arg;
  gboolean res;

  args = g_ptr_array_new ();
  g_ptr_array_add (args, (gchar *) argv0);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);

  res = xdp_spawnv (dir, output, flags, error, (const gchar * const *) args->pdata);

  g_ptr_array_free (args, TRUE);

  return res;
}

gboolean
xdp_spawnv (GFile                *dir,
            char                **output,
            GSubprocessFlags      flags,
            GError              **error,
            const gchar * const  *argv)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GInputStream *in;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  SpawnData data = {0};
  g_autofree gchar *commandline = NULL;

  launcher = g_subprocess_launcher_new (0);

  if (output)
    flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;

  g_subprocess_launcher_set_flags (launcher, flags);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  commandline = xdp_quote_argv ((const char **)argv);
  g_debug ("Running: %s", commandline);

  subp = g_subprocess_launcher_spawnv (launcher, argv, error);

  if (subp == NULL)
    return FALSE;

  loop = g_main_loop_new (NULL, FALSE);

  data.loop = loop;
  data.refs = 1;

  if (output)
    {
      data.refs++;
      in = g_subprocess_get_stdout_pipe (subp);
      out = g_memory_output_stream_new_resizable ();
      g_output_stream_splice_async (out,
                                    in,
                                    G_OUTPUT_STREAM_SPLICE_NONE,
                                    0,
                                    NULL,
                                    spawn_output_spliced_cb,
                                    &data);
    }

  g_subprocess_wait_async (subp, NULL, spawn_exit_cb, &data);

  g_main_loop_run (loop);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      g_clear_error (&data.splice_error);
      return FALSE;
    }

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, data.splice_error);
          return FALSE;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      g_output_stream_close (out, NULL, NULL);
      *output = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return TRUE;
}

char *
xdp_canonicalize_filename (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  return g_file_get_path (file);
}

gboolean
xdp_has_path_prefix (const char *str,
                     const char *prefix)
{
  while (TRUE)
    {
      /* Skip consecutive slashes to reach next path
         element */
      while (*str == '/')
        str++;
      while (*prefix == '/')
        prefix++;

      /* No more prefix path elements? Done! */
      if (*prefix == 0)
        return TRUE;

      /* Compare path element */
      while (*prefix != 0 && *prefix != '/')
        {
          if (*str != *prefix)
            return FALSE;
          str++;
          prefix++;
        }

      /* Matched prefix path element,
         must be entire str path element */
      if (*str != '/' && *str != 0)
        return FALSE;
    }
}

/* pid mapping code */
static int
parse_pid (const char *str,
           pid_t      *pid)
{
  char *end;
  guint64 v;
  pid_t p;

  errno = 0;
  v = g_ascii_strtoull (str, &end, 0);
  if (end == str)
    return -ENOENT;
  else if (errno != 0)
    return -errno;

  p = (pid_t) v;

  if (p < 1 || (guint64) p != v)
    return -ERANGE;

  if (pid)
    *pid = p;

  return 0;
}

static int
parse_status_field_pid (const char *val,
                        pid_t      *pid)
{
  const char *t;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  return parse_pid (t, pid);
}

static int
parse_status_field_uid (const char *val,
                        uid_t      *uid)
{
  const char *t;
  char *end;
  guint64 v;
  uid_t u;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  errno = 0;
  v = g_ascii_strtoull (t, &end, 0);
  if (end == val)
    return -ENOENT;
  else if (errno != 0)
    return -errno;

  u = (uid_t) v;

  if ((guint64) u != v)
    return -ERANGE;

  if (uid)
    *uid = u;

  return 0;
}

static int
parse_status_file (int    pid_fd,
                   pid_t *pid_out,
                   uid_t *uid_out)
{
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  gboolean have_pid = pid_out == NULL;
  gboolean have_uid = uid_out == NULL;
  FILE *f;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  int fd;
  int r = 0;

  g_return_val_if_fail (pid_fd > -1, FALSE);

  fd = openat (pid_fd, "status",  O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd == -1)
    return -errno;

  f = fdopen (fd, "r");

  if (f == NULL)
    return -errno;

  fd = -1; /* fd is now owned by f */

  do {
    n = getdelim (&key, &keylen, ':', f);
    if (n == -1)
      {
        r = -errno;
        break;
      }

    n = getdelim (&val, &vallen, '\n', f);
    if (n == -1)
      {
        r = -errno;
        break;
      }

    g_strstrip (key);
    g_strstrip (val);

    if (!strncmp (key, "NSpid", strlen ("NSpid")))
      {
        r = parse_status_field_pid (val, pid_out);
        have_pid = r > -1;
      }
    else if (!strncmp (key, "Uid", strlen ("Uid")))
      {
        r = parse_status_field_uid (val, uid_out);
        have_uid = r > -1;
      }

    if (r < 0)
      g_warning ("Failed to parse 'status::%s': %s",
                 key, g_strerror (-r));

  } while (r == 0 && (!have_uid || !have_pid));

  fclose (f);

  if (r != 0)
    return r;
  else if (!have_uid || !have_pid)
    return -ENXIO; /* ENOENT for the fields */

  return 0;
}

static int
lookup_ns_from_pid_fd (int    pid_fd,
                       ino_t *ns)
{
  struct stat st;
  int r;

  g_return_val_if_fail (ns != NULL, FALSE);

  r = fstatat (pid_fd, "ns/pid", &st, 0);
  if (r == -1)
    return -errno;

  /* The inode number (together with the device ID) encode
   * the identity of the pid namespace, see namespaces(7)
   */
  *ns = st.st_ino;

  return 0;
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

static int
open_fdinfo_dir (GError **error)
{
  int fd;

  fd = open ("/proc/self/fdinfo", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);

  if (fd < 0)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Could not to open /proc/self/fdinfo: %s",
                 g_strerror (errno));

  return fd;
}

static inline gboolean
find_pid (pid_t *pids,
          guint  n_pids,
          pid_t  want,
          guint *idx)
{
  for (guint i = 0; i < n_pids; i++)
    {
      if (pids[i] == want)
        {
          *idx = i;
          return TRUE;
        }
    }

  return FALSE;
}

static gboolean
map_pids (DIR     *proc,
          ino_t    pidns,
          pid_t   *pids,
          guint    n_pids,
          uid_t    target_uid,
          GError **error)
{
  pid_t *res = NULL;
  struct dirent *de;
  guint count = 0;

  res = g_alloca (sizeof (pid_t) * n_pids);
  memset (res, 0, sizeof (pid_t) * n_pids);

  while ((de = readdir (proc)) != NULL)
    {
      xdp_autofd int pid_fd = -1;
      pid_t outside = 0;
      pid_t inside = 0;
      uid_t uid = 0;
      guint idx;
      ino_t ns = 0;
      int r;

      if (de->d_type != DT_DIR)
        continue;

      pid_fd = openat (dirfd (proc), de->d_name, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
      if (pid_fd == -1)
        continue;

      r = lookup_ns_from_pid_fd (pid_fd, &ns);
      if (r < 0)
        continue;

      if (pidns != ns)
        continue;

      r = parse_pid (de->d_name, &outside);
      if (r < 0)
        continue;

      r = parse_status_file (pid_fd, &inside, &uid);
      if (r < 0)
        continue;

      if (!find_pid (pids, n_pids, inside, &idx))
        continue;

      /* We got a match, let's make sure the real uids match as well */
      if (uid != target_uid)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                               "Matching pid doesn't belong to the target user");
          return FALSE;
        }

      /* this handles the first occurrence, already identified by find_pid,
       * as well as duplicate entries */
      for (guint i = idx; i < n_pids; i++)
        {
          if (pids[i] == inside)
            {
              res[idx] = outside;
              count++;
            }
        }
    }

  if (count != n_pids)
    {
      g_autoptr(GString) str = NULL;

      str = g_string_new ("Process ids could not be found: ");

      for (guint i = 0; i < n_pids; i++)
        if (res[i] == 0)
          g_string_append_printf (str, "%d, ", (guint32) pids[i]);

      g_string_truncate (str, str->len - 2);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, str->str);

      return FALSE;
    }

  memcpy (pids, res, sizeof (pid_t) * n_pids);

  return TRUE;
}

static gboolean
pidfd_to_pid (int fdinfo, const int pidfd, pid_t *pid, GError **error)
{
  g_autofree char *name = NULL;
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  gboolean found = FALSE;
  FILE *f = NULL;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  int fd;
  int r = 0;

  *pid = 0;

  name = g_strdup_printf ("%d", pidfd);

  fd = openat (fdinfo, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);

  if (fd != -1)
    f = fdopen (fd, "r");

  if (f == NULL)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Unable to open /proc/self/fdinfo/%d: %s",
                   fd, g_strerror (errno));
      return FALSE;
    }

  do {
    n = getdelim (&key, &keylen, ':', f);
    if (n == -1)
      {
        r = errno;
        break;
      }

    n = getdelim (&val, &vallen, '\n', f);
    if (n == -1)
      {
        r = errno;
        break;
      }

    g_strstrip (key);

    if (!strncmp (key, "Pid", 3))
      {
        r = parse_status_field_pid (val, pid);
        found = r > -1;
      }

  } while (r == 0 && !found);

  fclose (f);

  if (r < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not parse fdinfo::%s: %s",
                   key, g_strerror (-r));
    }
  else if (!found)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not parse fdinfo: Pid field missing");
    }

  return found;
}

static JsonNode *
xdg_app_info_load_brwap_info (XdpAppInfo *app_info,
                              GError    **error)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonNode) root = NULL;
  g_autofree char *instance = NULL;
  g_autofree char *data = NULL;
  gsize len;
  char *path;

  g_return_val_if_fail (app_info != NULL, 0);

  instance = xdp_app_info_get_instance (app_info);

  if (instance == NULL)
    return 0;

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

static ino_t
xdp_app_info_get_pid_namespace (JsonNode *root,
                                GError  **error)
{
  JsonNode *node;
  JsonObject *cpo;
  gint64 nsid;

  /* xdg_app_info_load_brwap_info assures root is of type object */
  cpo = json_node_get_object (root);
  node = json_object_get_member (cpo, "pid-namespace");

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "pid-namespace missing");
      return 0;
    }

  nsid = json_node_get_int (node);
  return (ino_t) nsid;
}

static pid_t
xdp_app_info_get_child_pid (JsonNode *root,
                            GError  **error)
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

#define xdp_lockguard G_GNUC_UNUSED __attribute__((cleanup(xdp_auto_unlock_helper)))

static gboolean
xdg_app_info_ensure_pidns (XdpAppInfo  *app_info,
                           DIR         *proc,
                           GError     **error)
{
  g_autoptr(JsonNode) root = NULL;
  g_autoptr(GMutexLocker) guard = NULL;
  xdp_autofd int fd = -1;
  pid_t pid;
  ino_t ns;
  int r;

  g_assert (app_info->kind == XDP_APP_INFO_KIND_FLATPAK);

  guard = g_mutex_locker_new (&(app_info->u.flatpak.pidns_lock));

  if (app_info->u.flatpak.pidns_id != 0)
    return TRUE;

  root = xdg_app_info_load_brwap_info (app_info, error);
  if (root == NULL)
    return FALSE;

  /* newer versions of bubblewrap contain the namespace
   * information directly, so we don' thave to go via the
   * child-pid; if this fails, we fallback to the old way */
  ns = xdp_app_info_get_pid_namespace (root, NULL);
  if (ns != 0)
    {
      g_debug ("Using pid namespace info from bwrap info");
      app_info->u.flatpak.pidns_id = ns;
      return TRUE;
    }

  pid = xdp_app_info_get_child_pid (root, error);
  if (pid == 0)
    return FALSE;

  fd = open_pid_fd (dirfd (proc), pid, error);
  if (fd == -1)
    return FALSE;

  r = lookup_ns_from_pid_fd (fd, &ns);
  if (r < 0)
    {
      int code = g_io_error_from_errno (-r);
      g_set_error (error, G_IO_ERROR, code,
                   "Could not query /proc/%u/ns/pid: %s",
                   (guint) pid, g_strerror (-r));
      return FALSE;
    }

  app_info->u.flatpak.pidns_id = ns;

  return TRUE;
}



gboolean
xdg_app_info_map_pids (XdpAppInfo  *app_info,
                       pid_t       *pids,
                       guint        n_pids,
                       GError     **error)
{
  gboolean ok;
  DIR *proc;
  uid_t uid;
  ino_t ns;

  g_return_val_if_fail (app_info != NULL, FALSE);
  g_return_val_if_fail (pids != NULL, FALSE);

  if (app_info->kind != XDP_APP_INFO_KIND_FLATPAK)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Mapping pids is not supported.");
      return FALSE;
    }

  proc = opendir ("/proc");
  if (proc == NULL)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not open '/proc: %s", g_strerror (errno));
      return FALSE;
    }

  /* Make sure we know the pid namespace the app is running in */
  ok = xdg_app_info_ensure_pidns (app_info, proc, error);
  if (!ok)
    {
      g_prefix_error (error, "Could not determine pid namespace: ");
      goto out;
    }

  /* we also make sure the real user id matches
   * to the process owner we are trying to resolve
   */
  uid = getuid ();

  ns = app_info->u.flatpak.pidns_id;
  ok = map_pids (proc, ns, pids, n_pids, uid, error);

 out:
  closedir (proc);
  return ok;
}

gboolean
xdg_app_info_pidfds_to_pids (XdpAppInfo  *app_info,
                             const int   *fds,
                             pid_t       *pids,
                             gint         count,
                             GError     **error)
{
  gboolean ok = TRUE;
  int fdinfo = -1;

  g_return_val_if_fail (app_info != NULL, FALSE);
  g_return_val_if_fail (fds != NULL, FALSE);
  g_return_val_if_fail (pids != NULL, FALSE);

  fdinfo = open_fdinfo_dir (error);
  if (fdinfo == -1)
    return FALSE;

  for (gint i = 0; i < count && ok; i++)
    ok = pidfd_to_pid (fdinfo, fds[i], &pids[i], error);

  (void) close (fdinfo);

  return ok;
}
