/*
 * Copyright © 2024 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-login.h>
#include "sd-escape.h"
#endif

#include <json-glib/json-glib.h>
#include <gio/gunixfdlist.h>

#include "xdp-app-info-private.h"

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_KEY_APP_PATH "app-path"
#define FLATPAK_METADATA_KEY_ORIGINAL_APP_PATH "original-app-path"
#define FLATPAK_METADATA_KEY_RUNTIME_PATH "runtime-path"
#define FLATPAK_METADATA_KEY_INSTANCE_ID "instance-id"

#define SNAP_METADATA_GROUP_INFO "Snap Info"
#define SNAP_METADATA_KEY_INSTANCE_NAME "InstanceName"
#define SNAP_METADATA_KEY_DESKTOP_FILE "DesktopFile"
#define SNAP_METADATA_KEY_NETWORK "HasNetworkStatus"

G_LOCK_DEFINE (app_infos);
static GHashTable *app_info_by_unique_name;

G_DEFINE_QUARK (XdpAppInfo, xdp_app_info_error);

typedef enum
{
  XDP_APP_INFO_KIND_HOST = 0,
  XDP_APP_INFO_KIND_FLATPAK = 1,
  XDP_APP_INFO_KIND_SNAP    = 2,
} XdpAppInfoKind;

typedef struct _XdpAppInfoPrivate
{
  XdpAppInfoKind kind;

  char *engine;
  char *id;
  char *instance;
  int pidfd;
  GAppInfo *gappinfo;
  gboolean supports_opath;
  gboolean has_network;
  gboolean requires_pid_mapping;

  /* pid namespace mapping */
  GMutex pidns_lock;
  ino_t pidns_id;

  union
    {
      struct
        {
          GKeyFile *keyfile;
        } flatpak;
      struct
        {
          GKeyFile *keyfile;
        } snap;
    } u;
} XdpAppInfoPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (XdpAppInfo, xdp_app_info, G_TYPE_OBJECT)

static void
xdp_app_info_dispose (GObject *object)
{
  XdpAppInfoPrivate *priv =
    xdp_app_info_get_instance_private (XDP_APP_INFO (object));

  g_clear_pointer (&priv->engine, g_free);
  g_clear_pointer (&priv->id, g_free);
  g_clear_pointer (&priv->instance, g_free);
  xdp_close_fd (&priv->pidfd);
  g_clear_object (&priv->gappinfo);

  switch (priv->kind)
    {
    case XDP_APP_INFO_KIND_FLATPAK:
      g_clear_pointer (&priv->u.flatpak.keyfile, g_key_file_free);
      break;

    case XDP_APP_INFO_KIND_SNAP:
      g_clear_pointer (&priv->u.snap.keyfile, g_key_file_free);
      break;

    case XDP_APP_INFO_KIND_HOST:
    default:
      break;
    }

  G_OBJECT_CLASS (xdp_app_info_parent_class)->dispose (object);
}

static void
xdp_app_info_class_init (XdpAppInfoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_dispose;
}

static void
xdp_app_info_init (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  priv->pidfd = -1;
}

void
xdp_app_info_initialize (XdpAppInfo *app_info,
                         const char *engine,
                         const char *app_id,
                         const char *instance,
                         int         pidfd,
                         GAppInfo   *gappinfo,
                         gboolean    supports_opath,
                         gboolean    has_network,
                         gboolean    requires_pid_mapping)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  priv->engine = g_strdup (engine);
  priv->id = g_strdup (app_id);
  priv->instance = g_strdup (instance);
  priv->pidfd = dup (pidfd);
  g_set_object (&priv->gappinfo, gappinfo);
  priv->supports_opath = supports_opath;
  priv->has_network = has_network;
  priv->requires_pid_mapping = requires_pid_mapping;
}

static XdpAppInfo *
xdp_app_info_new (XdpAppInfoKind kind)
{
  XdpAppInfo *app_info;
  XdpAppInfoPrivate *priv;

  app_info = g_object_new (XDP_TYPE_APP_INFO, NULL);

  priv = xdp_app_info_get_instance_private (app_info);

  priv->kind = kind;
  priv->pidfd = -1;

  return app_info;
}

gboolean
xdp_app_info_is_host (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  g_return_val_if_fail (app_info != NULL, FALSE);

  return priv->kind == XDP_APP_INFO_KIND_HOST;
}

const char *
xdp_app_info_get_id (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  g_return_val_if_fail (app_info != NULL, NULL);

  return priv->id;
}

char *
xdp_app_info_get_instance (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  g_return_val_if_fail (app_info != NULL, NULL);

  if (priv->kind != XDP_APP_INFO_KIND_FLATPAK)
    return NULL;

  return g_key_file_get_string (priv->u.flatpak.keyfile,
                                FLATPAK_METADATA_GROUP_INSTANCE,
                                FLATPAK_METADATA_KEY_INSTANCE_ID,
                                NULL);
}

GAppInfo *
xdp_app_info_load_app_info (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
  g_autofree char *desktop_id = NULL;

  g_return_val_if_fail (app_info != NULL, NULL);

  switch (priv->kind)
    {
    case XDP_APP_INFO_KIND_FLATPAK:
      desktop_id = g_strconcat (priv->id, ".desktop", NULL);
      break;

    case XDP_APP_INFO_KIND_SNAP:
      desktop_id = g_key_file_get_string (priv->u.snap.keyfile,
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

gboolean
xdp_app_info_has_network (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
  gboolean has_network;

  switch (priv->kind)
    {
    case XDP_APP_INFO_KIND_FLATPAK:
      {
        g_auto(GStrv) shared = g_key_file_get_string_list (priv->u.flatpak.keyfile,
                                                           "Context", "shared",
                                                           NULL, NULL);
        if (shared)
          has_network = g_strv_contains ((const char * const *)shared, "network");
        else
          has_network = FALSE;
      }
      break;

    case XDP_APP_INFO_KIND_SNAP:
      has_network = g_key_file_get_boolean (priv->u.snap.keyfile,
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

gboolean
xdp_app_info_get_pidns (XdpAppInfo  *app_info,
                        ino_t       *pidns_id_out,
                        GError     **error)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
  g_autoptr(GMutexLocker) guard = g_mutex_locker_new (&(priv->pidns_lock));
  ino_t ns;

  *pidns_id_out = 0;

  if (priv->pidns_id != 0)
    {
      *pidns_id_out = priv->pidns_id;
      return TRUE;
    }

  if (priv->pidfd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "pidns required but no pidfd provided");
      return FALSE;
    }

  if (!xdp_pidfd_get_namespace (priv->pidfd, &ns, error))
    return FALSE;

  priv->pidns_id = ns;
  *pidns_id_out = ns;
  return TRUE;
}

static gboolean
xdp_app_info_supports_opath (XdpAppInfo  *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  return
    priv->kind == XDP_APP_INFO_KIND_FLATPAK ||
    priv->kind == XDP_APP_INFO_KIND_HOST;
}

static char *
verify_proc_self_fd (XdpAppInfo  *app_info,
                     const char  *proc_path,
                     GError     **error)
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
     files are considered invalid. Unfortunately this also
     matches files with filenames that actually end in " (deleted)",
     but there is not much to do about this. */
  if (g_str_has_suffix (path_buffer, " (deleted)"))
    {
      const char *mountpoint = xdp_get_documents_mountpoint ();

      if (mountpoint != NULL && g_str_has_prefix (path_buffer, mountpoint))
        {
          /* Unfortunately our workaround for dcache purging triggers
             o_path file descriptors on the fuse filesystem being
             marked as deleted, so we have to allow these here and
             rewrite them. This is safe, becase we will stat the file
             and compare to make sure we end up on the right file. */
          path_buffer[symlink_size - strlen(" (deleted)")] = 0;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                       "Cannot share deleted file: %s", path_buffer);
          return NULL;
        }
    }

  /* remap from sandbox to host if needed */
  return xdp_app_info_remap_path (app_info, path_buffer);
}

static gboolean
check_same_file (const char   *path,
                 struct stat  *expected_st_buf,
                 GError      **error)
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
xdp_app_info_get_path_for_fd (XdpAppInfo   *app_info,
                              int           fd,
                              int           require_st_mode,
                              struct stat  *st_buf,
                              gboolean     *writable_out,
                              GError      **error)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
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
                       priv->id, priv->kind);
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

char *
xdp_app_info_remap_path (XdpAppInfo *app_info,
                         const char *path)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  if (priv->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
      g_autofree char *app_path = g_key_file_get_string (priv->u.flatpak.keyfile,
                                                         FLATPAK_METADATA_GROUP_INSTANCE,
                                                         FLATPAK_METADATA_KEY_APP_PATH, NULL);
      g_autofree char *runtime_path = g_key_file_get_string (priv->u.flatpak.keyfile,
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
                                 priv->id, "config",
                                 path + strlen ("/var/config/"), NULL);
      else if (g_str_has_prefix (path, "/var/data/"))
        return g_build_filename (g_get_home_dir (), ".var", "app",
                                 priv->id, "data",
                                 path + strlen ("/var/data/"), NULL);
    }

  return g_strdup (path);
}

static char **
rewrite_commandline (XdpAppInfo         *app_info,
                     const char * const *commandline,
                     gboolean            quote_escape)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  g_autoptr(GPtrArray) args = NULL;

  g_return_val_if_fail (app_info != NULL, NULL);

  if (priv->kind == XDP_APP_INFO_KIND_HOST)
    {
      int i;
      args = g_ptr_array_new_with_free_func (g_free);
      for (i = 0; commandline && commandline[i]; i++)
        g_ptr_array_add (args, xdp_maybe_quote (commandline[i], quote_escape));
      g_ptr_array_add (args, NULL);
      return (char **)g_ptr_array_free (g_steal_pointer (&args), FALSE);
    }
  else if (priv->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
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
            g_ptr_array_add (args, g_shell_quote (priv->id));
          else
            g_ptr_array_add (args, g_strdup (priv->id));

          for (i = 1; commandline[i]; i++)
            g_ptr_array_add (args, xdp_maybe_quote (commandline[i], quote_escape));
        }
      else if (quote_escape)
        g_ptr_array_add (args, g_shell_quote (priv->id));
      else
        g_ptr_array_add (args, g_strdup (priv->id));
      g_ptr_array_add (args, NULL);

      return (char **)g_ptr_array_free (g_steal_pointer (&args), FALSE);
    }
  else
    return NULL;
}

static char *
get_tryexec_path (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  g_return_val_if_fail (app_info != NULL, NULL);

  if (priv->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
      g_autofree char *original_app_path = NULL;
      g_autofree char *tryexec_path = NULL;
      g_autofree char *app_slash = NULL;
      g_autofree char *app_path = NULL;
      char *app_slash_pointer;
      char *path;

      original_app_path = g_key_file_get_string (priv->u.flatpak.keyfile,
                                                 FLATPAK_METADATA_GROUP_INSTANCE,
                                                 FLATPAK_METADATA_KEY_ORIGINAL_APP_PATH, NULL);
      app_path = g_key_file_get_string (priv->u.flatpak.keyfile,
                                        FLATPAK_METADATA_GROUP_INSTANCE,
                                        FLATPAK_METADATA_KEY_APP_PATH, NULL);
      path = original_app_path ? original_app_path : app_path;

      if (path == NULL || *path == '\0')
        return NULL;

      app_slash = g_strconcat ("app/", priv->id, NULL);

      app_slash_pointer = strstr (path, app_slash);
      if (app_slash_pointer == NULL)
        return NULL;

      /* Terminate path after the flatpak installation path such as .local/share/flatpak/ */
      *app_slash_pointer = '\0';

      /* Find the path to the wrapper script exported by Flatpak, which can be
       * used in a desktop file's TryExec=
       */
      tryexec_path = g_strconcat (path, "exports/bin/", priv->id, NULL);
      if (access (tryexec_path, X_OK) != 0)
        {
          g_debug ("Wrapper script unexpectedly not executable or nonexistent: %s", tryexec_path);
          return NULL;
        }

      return g_steal_pointer (&tryexec_path);
    }
  else
    return NULL;
}

gboolean
xdp_app_info_validate_autostart (XdpAppInfo          *app_info,
                                 GKeyFile            *keyfile,
                                 const char * const  *autostart_exec,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  g_auto(GStrv) cmdv = NULL;
  g_autofree char *cmd = NULL;

  g_assert (priv->id);

  cmdv = rewrite_commandline (app_info,
                              autostart_exec,
                              FALSE /* don't quote escape */);
  if (!cmdv)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Autostart not supported for: %s", priv->id);
      return FALSE;
    }

  cmd = g_strjoinv (" ", cmdv);

  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_EXEC,
                         cmd);

  if (priv->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
      g_key_file_set_string (keyfile,
                             G_KEY_FILE_DESKTOP_GROUP,
                             "X-Flatpak",
                             priv->id);
    }

  return TRUE;
}

gboolean
xdp_app_info_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                        GKeyFile    *key_file,
                                        GError     **error)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
  g_autofree char *exec = NULL;
  g_auto(GStrv) exec_strv = NULL;
  g_auto(GStrv) prefixed_exec_strv = NULL;
  g_autofree char *prefixed_exec = NULL;
  g_autofree char *tryexec_path = NULL;
  const char *app_id = xdp_app_info_get_id (app_info);

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
  if (priv->kind == XDP_APP_INFO_KIND_FLATPAK &&
      g_strv_contains ((const char * const *)exec_strv, "--file-forwarding"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Desktop entry given to Install() must not use --file-forwarding");
      return FALSE;
    }

  prefixed_exec_strv = rewrite_commandline (app_info,
                                            (const char * const *)exec_strv,
                                            TRUE /* quote escape */);
  if (prefixed_exec_strv == NULL)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   "DynamicLauncher install not supported for: %s", app_id);
      return FALSE;
    }

  prefixed_exec = g_strjoinv (" ", prefixed_exec_strv);
  g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "Exec", prefixed_exec);

  tryexec_path = get_tryexec_path (app_info);
  if (tryexec_path != NULL)
    g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "TryExec", tryexec_path);

  if (priv->kind == XDP_APP_INFO_KIND_FLATPAK)
    {
      /* Flatpak checks for this key */
      g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", app_id);
      /* Flatpak removes this one for security */
      g_key_file_remove_key (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-GNOME-Bugzilla-ExtraInfoScript", NULL);
    }

  return TRUE;
}

#ifdef HAVE_LIBSYSTEMD
char *
_xdp_parse_app_id_from_unit_name (const char *unit)
{
  g_autoptr(GRegex) regex1 = NULL;
  g_autoptr(GRegex) regex2 = NULL;
  g_autoptr(GMatchInfo) match = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *app_id = NULL;

  g_assert (g_str_has_prefix (unit, "app-"));

  /*
   * From https://systemd.io/DESKTOP_ENVIRONMENTS/ the format is one of:
   * app[-<launcher>]-<ApplicationID>-<RANDOM>.scope
   * app[-<launcher>]-<ApplicationID>-<RANDOM>.slice
   */
  regex1 = g_regex_new ("^app-(?:[[:alnum:]]+\\-)?(.+?)(?:\\-[[:alnum:]]*)(?:\\.scope|\\.slice)$", 0, 0, &error);
  g_assert (error == NULL);
  /*
   * app[-<launcher>]-<ApplicationID>-autostart.service -> no longer true since systemd v248
   * app[-<launcher>]-<ApplicationID>[@<RANDOM>].service
   */
  regex2 = g_regex_new ("^app-(?:[[:alnum:]]+\\-)?(.+?)(?:@[[:alnum:]]*|\\-autostart)?\\.service$", 0, 0, &error);
  g_assert (error == NULL);

  if (!g_regex_match (regex1, unit, 0, &match))
    g_clear_pointer (&match, g_match_info_unref);

  if (match == NULL && !g_regex_match (regex2, unit, 0, &match))
    g_clear_pointer (&match, g_match_info_unref);

  if (match != NULL)
    {
      g_autofree char *escaped_app_id = NULL;
      /* Unescape the unit name which may have \x hex codes in it, e.g.
       * "app-gnome-org.gnome.Evolution\x2dalarm\x2dnotify-2437.scope"
       */
      escaped_app_id = g_match_info_fetch (match, 1);
      if (cunescape (escaped_app_id, UNESCAPE_RELAX, &app_id) < 0)
        app_id = g_strdup ("");
    }
  else
    {
      app_id = g_strdup ("");
    }

  return g_steal_pointer (&app_id);
}
#endif /* HAVE_LIBSYSTEMD */

static void
set_appid_from_pid (XdpAppInfo *app_info,
                    pid_t       pid)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

#ifdef HAVE_LIBSYSTEMD
  g_autofree char *unit = NULL;
  int res;

  g_return_if_fail (priv->id == NULL);

  res = sd_pid_get_user_unit (pid, &unit);
  /*
   * The session might not be managed by systemd or there could be an error
   * fetching our own systemd units or the unit might not be started by the
   * desktop environment (e.g. it's a script run from terminal).
   */
  if (res == -ENODATA || res < 0 || !unit || !g_str_has_prefix (unit, "app-"))
    {
      priv->id = g_strdup ("");
      return;
    }

  priv->id = _xdp_parse_app_id_from_unit_name (unit);
  g_debug ("Assigning app ID \"%s\" to pid %ld which has unit \"%s\"",
           priv->id, (long) pid, unit);

#else
  priv->id = g_strdup ("");
#endif /* HAVE_LIBSYSTEMD */
}

static gboolean
xdp_app_info_from_flatpak (int          pid,
                           XdpAppInfo **out_app_info,
                           GError     **error)
{
  XdpAppInfoPrivate *priv;
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
            {
              *out_app_info = NULL;
              return TRUE;
            }
        }

      /* Otherwise, we should be able to open the root dir. Probably the app died and
         we're failing due to /proc/$pid not existing. In that case fail instead
         of treating this as privileged. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open %s", root_path);
      return FALSE;
    }

  metadata = g_key_file_new ();

  info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  close (root_fd);
  if (info_fd == -1)
    {
      if (errno == ENOENT)
        {
          /* No file => on the host, return success */
          *out_app_info = NULL;
          return TRUE;
        }

      /* Some weird error => failure */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return FALSE;
    }

  if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    {
      /* Some weird fd => failure */
      close (info_fd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return FALSE;
    }

  mapped = g_mapped_file_new_from_fd  (info_fd, FALSE, &local_error);
  if (mapped == NULL)
    {
      close (info_fd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't map .flatpak-info file: %s", local_error->message);
      return FALSE;
    }

  if (!g_key_file_load_from_data (metadata,
                                  g_mapped_file_get_contents (mapped),
                                  g_mapped_file_get_length (mapped),
                                  G_KEY_FILE_NONE, &local_error))
    {
      close (info_fd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't load .flatpak-info file: %s", local_error->message);
      return FALSE;
    }

  group = "Application";
  if (g_key_file_has_group (metadata, "Runtime"))
    group = "Runtime";

  id = g_key_file_get_string (metadata, group, "name", error);
  if (id == NULL || !xdp_is_valid_app_id (id))
    {
      close (info_fd);
      return FALSE;
    }

  close (info_fd);

  app_info = xdp_app_info_new (XDP_APP_INFO_KIND_FLATPAK);

  priv = xdp_app_info_get_instance_private (app_info);

  priv->id = g_steal_pointer (&id);
  priv->u.flatpak.keyfile = g_steal_pointer (&metadata);

  *out_app_info = g_steal_pointer (&app_info);
  return TRUE;
}

int
_xdp_parse_cgroup_file (FILE     *f,
                        gboolean *is_snap)
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
      if ((strcmp (controller, "freezer:") == 0 ||
           strcmp (controller, "name=systemd:") == 0 ||
           strcmp (controller, ":") == 0) &&
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
pid_is_snap (pid_t    pid,
             GError **error)
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

static gboolean
xdp_app_info_from_snap (int          pid,
                        int          pidfd,
                        XdpAppInfo **out_app_info,
                        GError     **error)
{
  XdpAppInfoPrivate *priv;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *pid_str = NULL;
  g_autofree char *output = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autofree char *snap_name = NULL;

  /* Check the process's cgroup membership to fail quickly for non-snaps */
  if (!pid_is_snap (pid, error))
    {
      *out_app_info = NULL;
      return TRUE;
    }

  pid_str = g_strdup_printf ("%u", (guint) pid);
  output = xdp_spawn (error, "snap", "routine", "portal-info", pid_str, NULL);
  if (output == NULL)
    {
      return FALSE;
    }

  metadata = g_key_file_new ();
  if (!g_key_file_load_from_data (metadata, output, -1, G_KEY_FILE_NONE, &local_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't read snap info for pid %u: %s", pid, local_error->message);
      return FALSE;
    }

  snap_name = g_key_file_get_string (metadata, SNAP_METADATA_GROUP_INFO,
                                     SNAP_METADATA_KEY_INSTANCE_NAME, error);
  if (snap_name == NULL)
    {
      return FALSE;
    }

  app_info = xdp_app_info_new (XDP_APP_INFO_KIND_SNAP);

  priv = xdp_app_info_get_instance_private (app_info);

  priv->id = g_strconcat ("snap.", snap_name, NULL);
  priv->pidfd = dup (pidfd);
  priv->u.snap.keyfile = g_steal_pointer (&metadata);

  *out_app_info = g_steal_pointer (&app_info);
  return TRUE;
}

static XdpAppInfo *
xdp_app_info_from_host (pid_t pid,
                        int   pidfd)
{
  XdpAppInfoPrivate *priv;
  XdpAppInfo *app_info;

  app_info = xdp_app_info_new (XDP_APP_INFO_KIND_HOST);
  priv = xdp_app_info_get_instance_private (app_info);

  set_appid_from_pid (app_info, pid);
  priv->pidfd = dup (pidfd);
  return app_info;
}

static XdpAppInfo *
xdp_app_info_new_test_host (const char *app_id)
{
  XdpAppInfoPrivate *priv;
  XdpAppInfo *app_info;

  app_info = xdp_app_info_new (XDP_APP_INFO_KIND_HOST);
  priv = xdp_app_info_get_instance_private (app_info);

  priv->id = g_strdup (app_id);
  return app_info;
}

static gboolean
xdp_connection_get_pid_legacy (GDBusConnection  *connection,
                               const char       *sender,
                               GCancellable     *cancellable,
                               int              *out_pidfd,
                               uint32_t         *out_pid,
                               GError          **error)
{
  g_autoptr(GVariant) reply = NULL;

  reply = g_dbus_connection_call_sync (connection,
                                       DBUS_NAME_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "GetConnectionUnixProcessID",
                                       g_variant_new ("(s)", sender),
                                       G_VARIANT_TYPE ("(u)"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       30000,
                                       cancellable,
                                       error);
  if (!reply)
    return FALSE;

  *out_pidfd = -1;
  g_variant_get (reply, "(u)", out_pid);
  return TRUE;
}

static gboolean
xdp_connection_get_pidfd (GDBusConnection  *connection,
                          const char       *sender,
                          GCancellable     *cancellable,
                          int              *out_pidfd,
                          uint32_t         *out_pid,
                          GError          **error)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) dict = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) process_fd = NULL;
  g_autoptr(GVariant) process_id = NULL;
  uint32_t pid;
  int fd_index;
  GUnixFDList *fd_list;
  int fds_len = 0;
  const int *fds;
  int pidfd;

  reply = g_dbus_connection_call_with_unix_fd_list_sync (connection,
                                                         DBUS_NAME_DBUS,
                                                         DBUS_PATH_DBUS,
                                                         DBUS_INTERFACE_DBUS,
                                                         "GetConnectionCredentials",
                                                         g_variant_new ("(s)", sender),
                                                         G_VARIANT_TYPE ("(a{sv})"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         30000,
                                                         NULL,
                                                         &fd_list,
                                                         cancellable,
                                                         &local_error);

  if (!reply)
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE))
        {
          return xdp_connection_get_pid_legacy (connection,
                                                sender,
                                                cancellable,
                                                out_pidfd,
                                                out_pid,
                                                error);
        }

      g_propagate_error (error, local_error);
      return FALSE;
    }

  g_variant_get (reply, "(@a{sv})", &dict);

  process_id = g_variant_lookup_value (dict, "ProcessID", G_VARIANT_TYPE_UINT32);
  if (!process_id)
    {
      return xdp_connection_get_pid_legacy (connection,
                                            sender,
                                            cancellable,
                                            out_pidfd,
                                            out_pid,
                                            error);
    }

  pid = g_variant_get_uint32 (process_id);

  process_fd = g_variant_lookup_value (dict, "ProcessFD", G_VARIANT_TYPE_HANDLE);
  if (!process_fd)
    {
      *out_pidfd = -1;
      *out_pid = pid;
      return TRUE;
    }

  fd_index = g_variant_get_handle (process_fd);

  if (fd_list == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer pidfd");
      return FALSE;
    }

  fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
  if (fds_len <= fd_index)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer pidfd");
      return FALSE;
    }

  pidfd = dup (fds[fd_index]);
  if (pidfd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't dup pidfd");
      return FALSE;
    }

  *out_pidfd = pidfd;
  *out_pid = pid;
  return TRUE;
}

static XdpAppInfo *
cache_lookup_app_info_by_sender (const char *sender)
{
  XdpAppInfo *app_info = NULL;

  G_LOCK (app_infos);
  if (app_info_by_unique_name)
    {
      app_info = g_hash_table_lookup (app_info_by_unique_name, sender);
      if (app_info)
        g_object_ref (app_info);
    }
  G_UNLOCK (app_infos);

  return app_info;
}

static void
ensure_app_info_by_unique_name (void)
{
  if (app_info_by_unique_name == NULL)
    app_info_by_unique_name = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free,
                                                     g_object_unref);
}

static void
cache_insert_app_info (const char *sender,
                       XdpAppInfo *app_info)
{
  G_LOCK (app_infos);
  ensure_app_info_by_unique_name ();
  g_hash_table_insert (app_info_by_unique_name, g_strdup (sender),
                       g_object_ref (app_info));
  G_UNLOCK (app_infos);
}

static void
on_peer_died (const char *name)
{
  G_LOCK (app_infos);
  if (app_info_by_unique_name)
    g_hash_table_remove (app_info_by_unique_name, name);
  G_UNLOCK (app_infos);
}

static XdpAppInfo *
xdp_connection_lookup_app_info_sync (GDBusConnection  *connection,
                                     const char       *sender,
                                     GCancellable     *cancellable,
                                     GError          **error)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  xdp_autofd int pidfd = -1;
  uint32_t pid;

  app_info = cache_lookup_app_info_by_sender (sender);
  if (app_info)
    return g_steal_pointer (&app_info);

  if (!xdp_connection_get_pidfd (connection, sender, cancellable, &pidfd, &pid, error))
    return NULL;

  if (!xdp_app_info_from_flatpak (pid, &app_info, error))
    return NULL;

  if (app_info == NULL && !xdp_app_info_from_snap (pid, pidfd, &app_info, error))
    return NULL;

  if (app_info == NULL)
    app_info = xdp_app_info_from_host (pid, pidfd);

  cache_insert_app_info (sender, app_info);

  xdp_connection_track_name_owners (connection, on_peer_died);

  return g_steal_pointer (&app_info);
}

XdpAppInfo *
xdp_invocation_lookup_app_info_sync (GDBusMethodInvocation  *invocation,
                                     GCancellable           *cancellable,
                                     GError                **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  const char *test_override_app_id;

  test_override_app_id = g_getenv ("XDG_DESKTOP_PORTAL_TEST_APP_ID");
  if (test_override_app_id)
    return xdp_app_info_new_test_host (test_override_app_id);

  return xdp_connection_lookup_app_info_sync (connection, sender, cancellable, error);
}
