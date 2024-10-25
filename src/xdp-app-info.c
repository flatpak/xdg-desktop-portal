/*
 * Copyright © 2024 Red Hat, Inc
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
#include "xdp-app-info-flatpak-private.h"
#include "xdp-app-info-snap-private.h"
#include "xdp-app-info-host-private.h"
#include "xdp-app-info-test-private.h"

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

G_LOCK_DEFINE (app_infos);
static GHashTable *app_info_by_unique_name;

G_DEFINE_QUARK (XdpAppInfo, xdp_app_info_error);

typedef struct _XdpAppInfoPrivate
{
  char *engine;
  char *id;
  char *instance;
  int pidfd;
  char *desktop_file_id;
  GAppInfo *gappinfo;
  gboolean supports_opath;
  gboolean has_network;
  gboolean requires_pid_mapping;

  /* pid namespace mapping */
  GMutex pidns_lock;
  ino_t pidns_id;
} XdpAppInfoPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (XdpAppInfo, xdp_app_info, G_TYPE_OBJECT)

static void
xdp_app_info_dispose (GObject *object)
{
  XdpAppInfoPrivate *priv =
    xdp_app_info_get_instance_private (XDP_APP_INFO (object));

  g_clear_pointer (&priv->engine, g_free);
  g_clear_pointer (&priv->id, g_free);
  g_clear_pointer (&priv->instance, g_free);
  g_clear_pointer (&priv->desktop_file_id, g_free);
  xdp_close_fd (&priv->pidfd);
  g_clear_object (&priv->gappinfo);

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
                         const char *desktop_file_id,
                         gboolean    supports_opath,
                         gboolean    has_network,
                         gboolean    requires_pid_mapping)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
  g_autoptr(GAppInfo) gappinfo = NULL;

  if (desktop_file_id != NULL)
    {
      g_autoptr(GDesktopAppInfo) desktop_appinfo = NULL;

      desktop_appinfo = g_desktop_app_info_new (desktop_file_id);
      if (desktop_appinfo)
        gappinfo = G_APP_INFO (g_steal_pointer (&desktop_appinfo));
    }

  priv->engine = g_strdup (engine);
  priv->id = g_strdup (app_id);
  priv->instance = g_strdup (instance);
  priv->pidfd = dup (pidfd);
  priv->desktop_file_id = g_strdup (desktop_file_id);
  g_set_object (&priv->gappinfo, g_steal_pointer (&gappinfo));
  priv->supports_opath = supports_opath;
  priv->has_network = has_network;
  priv->requires_pid_mapping = requires_pid_mapping;
}

gboolean
xdp_app_info_is_host (XdpAppInfo *app_info)
{
  return XDP_IS_APP_INFO_HOST (app_info) ||  XDP_IS_APP_INFO_TEST (app_info);
}

const char *
xdp_app_info_get_id (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, NULL);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->id;
}

const char *
xdp_app_info_get_instance (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, NULL);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->instance;
}

const char *
xdp_app_info_get_desktop_file_id (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, NULL);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->desktop_file_id;
}

GAppInfo *
xdp_app_info_get_gappinfo (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, NULL);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->gappinfo;
}

gboolean
xdp_app_info_is_valid_sub_app_id (XdpAppInfo *app_info,
                                  const char *sub_app_id)
{
  if (!XDP_APP_INFO_GET_CLASS (app_info)->is_valid_sub_app_id)
      return FALSE;

  return XDP_APP_INFO_GET_CLASS (app_info)->is_valid_sub_app_id (app_info,
                                                                 sub_app_id);
}

gboolean
xdp_app_info_has_network (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, TRUE);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->has_network;
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

static char *
remap_path (XdpAppInfo *app_info,
            const char *path)
{

  if (!XDP_APP_INFO_GET_CLASS (app_info)->remap_path)
    return g_strdup (path);

  return XDP_APP_INFO_GET_CLASS (app_info)->remap_path (app_info, path);
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
  return remap_path (app_info, path_buffer);
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

      if (!priv->supports_opath)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "App \"%s\" of type %s does not support O_PATH fd passing",
                       priv->id, priv->engine);
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

gboolean
xdp_app_info_validate_autostart (XdpAppInfo          *app_info,
                                 GKeyFile            *keyfile,
                                 const char * const  *autostart_exec,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  if (!priv->id ||
      !XDP_APP_INFO_GET_CLASS (app_info)->validate_autostart)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Autostart not supported for: %s", priv->id);
      return FALSE;
    }

  return XDP_APP_INFO_GET_CLASS (app_info)->validate_autostart (app_info,
                                                                keyfile,
                                                                autostart_exec,
                                                                cancellable,
                                                                error);
}

gboolean
xdp_app_info_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                        GKeyFile    *key_file,
                                        GError     **error)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  if (!priv->id ||
      !XDP_APP_INFO_GET_CLASS (app_info)->validate_dynamic_launcher)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "DynamicLauncher install not supported for: %s", priv->id);
      return FALSE;
    }

  return XDP_APP_INFO_GET_CLASS (app_info)->validate_dynamic_launcher (app_info,
                                                                       key_file,
                                                                       error);
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
  g_autoptr(GUnixFDList) fd_list = NULL;
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

      g_propagate_error (error, g_steal_pointer (&local_error));
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
  const char *test_override_app_id;
  g_autoptr(GError) local_error = NULL;

  app_info = cache_lookup_app_info_by_sender (sender);
  if (app_info)
    return g_steal_pointer (&app_info);

  if (!xdp_connection_get_pidfd (connection, sender, cancellable, &pidfd, &pid, error))
    return NULL;

  test_override_app_id = g_getenv ("XDG_DESKTOP_PORTAL_TEST_APP_ID");
  if (test_override_app_id)
    app_info = xdp_app_info_test_new (test_override_app_id);

  if (app_info == NULL)
    app_info = xdp_app_info_flatpak_new (pid, pidfd, &local_error);

  if (!app_info && !g_error_matches (local_error, XDP_APP_INFO_ERROR,
                                     XDP_APP_INFO_ERROR_WRONG_APP_KIND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  g_clear_error (&local_error);

  if (app_info == NULL)
    app_info = xdp_app_info_snap_new (pid, pidfd, &local_error);

  if (!app_info && !g_error_matches (local_error, XDP_APP_INFO_ERROR,
                                     XDP_APP_INFO_ERROR_WRONG_APP_KIND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  g_clear_error (&local_error);

  if (app_info == NULL)
    app_info = xdp_app_info_host_new (pid, pidfd);

  g_return_val_if_fail (app_info != NULL, NULL);

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

  return xdp_connection_lookup_app_info_sync (connection, sender, cancellable, error);
}
