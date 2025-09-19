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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif
#if HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#if HAVE_LIBSYSTEMD
#include <systemd/sd-login.h>
#include "sd-escape.h"
#endif

#include <json-glib/json-glib.h>
#include <gio/gunixfdlist.h>

#include "xdp-app-info-private.h"
#include "xdp-app-info-flatpak-private.h"
#include "xdp-app-info-snap-private.h"
#include "xdp-app-info-host-private.h"
#include "xdp-enum-types.h"
#include "xdp-utils.h"


G_LOCK_DEFINE (app_infos);
static GHashTable *app_info_by_unique_name;

G_DEFINE_QUARK (XdpAppInfo, xdp_app_info_error);

typedef struct _XdpAppInfoPrivate
{
  /* identity */
  char *engine;
  char *id;
  char *instance;

  /* app info */
  GAppInfo *gappinfo;

  /* calling process */
  int pidfd;

  /* pid namespace mapping */
  GMutex pidns_lock;
  ino_t pidns_id;

  /* misc */
  XdpAppInfoFlags flags;
} XdpAppInfoPrivate;

static void g_initable_init_iface (GInitableIface *iface);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (XdpAppInfo, xdp_app_info, G_TYPE_OBJECT,
                                  G_ADD_PRIVATE (XdpAppInfo)
                                  G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, g_initable_init_iface))

enum
{
  PROP_0,
  PROP_ENGINE,
  PROP_FLAGS,
  PROP_G_APP_INFO,
  PROP_ID,
  PROP_INSTANCE,
  PROP_PIDFD,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

static gboolean
xdp_app_info_initable_init (GInitable     *initable,
                            GCancellable  *cancellable,
                            GError       **error)
{
  XdpAppInfo *app_info = XDP_APP_INFO (initable);
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  priv->gappinfo =
    XDP_APP_INFO_GET_CLASS (app_info)->create_gappinfo (app_info);

  if ((priv->flags & XDP_APP_INFO_FLAG_REQUIRE_GAPPINFO) && !priv->gappinfo)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "App info not found for '%s'", priv->id);
      return FALSE;
    }

  return TRUE;
}

static void
g_initable_init_iface (GInitableIface *iface)
{
  iface->init = xdp_app_info_initable_init;
}

static GAppInfo *
xdp_app_info_real_create_gappinfo (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);
  g_autoptr(GAppInfo) gappinfo = NULL;
  g_autofree char *desktop_id = NULL;

  desktop_id = g_strconcat (priv->id, ".desktop", NULL);
  gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  return g_steal_pointer (&gappinfo);
}

static void
xdp_app_info_dispose (GObject *object)
{
  XdpAppInfoPrivate *priv =
    xdp_app_info_get_instance_private (XDP_APP_INFO (object));
  g_autoptr(GError) error = NULL;

  g_clear_pointer (&priv->engine, g_free);
  g_clear_pointer (&priv->id, g_free);
  g_clear_pointer (&priv->instance, g_free);
  g_clear_object (&priv->gappinfo);

  if (!g_clear_fd (&priv->pidfd, &error))
    g_warning ("Error closing pidfd: %s", error->message);

  G_OBJECT_CLASS (xdp_app_info_parent_class)->dispose (object);
}

static void
xdp_app_info_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  XdpAppInfoPrivate *priv =
    xdp_app_info_get_instance_private (XDP_APP_INFO (object));

  switch (prop_id)
    {
    case PROP_ENGINE:
      g_value_set_string (value, priv->engine);
      break;

    case PROP_FLAGS:
      g_value_set_flags (value, priv->flags);
      break;

    case PROP_G_APP_INFO:
      g_value_set_object (value, priv->gappinfo);
      break;

    case PROP_ID:
      g_value_set_string (value, priv->id);
      break;

    case PROP_INSTANCE:
      g_value_set_string (value, priv->instance);
      break;

    case PROP_PIDFD:
      g_value_set_int (value, priv->pidfd);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
xdp_app_info_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  XdpAppInfoPrivate *priv =
    xdp_app_info_get_instance_private (XDP_APP_INFO (object));

  switch (prop_id)
    {
    case PROP_ENGINE:
      g_assert (priv->engine == NULL);
      priv->engine = g_value_dup_string (value);
      break;

    case PROP_FLAGS:
      g_assert (priv->flags == 0);
      priv->flags = g_value_get_flags (value);
      break;

    case PROP_ID:
      g_assert (priv->id == NULL);
      priv->id = g_value_dup_string (value);
      break;

    case PROP_INSTANCE:
      g_assert (priv->instance == NULL);
      priv->instance = g_value_dup_string (value);
      break;

    case PROP_PIDFD:
      g_assert (priv->pidfd == -1);
      /* Steals ownership from the GValue */
      priv->pidfd = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
xdp_app_info_class_init (XdpAppInfoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_dispose;
  object_class->get_property = xdp_app_info_get_property;
  object_class->set_property = xdp_app_info_set_property;

  klass->create_gappinfo = xdp_app_info_real_create_gappinfo;

  properties[PROP_ENGINE] =
    g_param_spec_string ("engine", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  properties[PROP_FLAGS] =
    g_param_spec_flags ("flags", NULL, NULL,
                        XDP_TYPE_APP_INFO_FLAGS,
                        0,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  properties[PROP_G_APP_INFO] =
    g_param_spec_object ("g-app-info", NULL, NULL,
                         G_TYPE_APP_INFO,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ID] =
    g_param_spec_string ("id", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  properties[PROP_INSTANCE] =
    g_param_spec_string ("instance", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /* Note that setting this property at construct-time takes ownership
   * of the fd from the caller */
  properties[PROP_PIDFD] =
    g_param_spec_int ("pidfd", NULL, NULL,
                      -1, G_MAXINT, -1,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
xdp_app_info_init (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  priv->pidfd = -1;
}

static XdpAppInfo *
xdp_app_info_new (uint32_t   pid,
                  int        pidfd,
                  GError   **error)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autofd int pidfd_owned = pidfd;
  g_autoptr(GError) local_error = NULL;

  app_info = xdp_app_info_flatpak_new (pid, &pidfd_owned, &local_error);

  if (!app_info && !g_error_matches (local_error, XDP_APP_INFO_ERROR,
                                     XDP_APP_INFO_ERROR_WRONG_APP_KIND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  g_clear_error (&local_error);

  if (app_info == NULL)
    app_info = xdp_app_info_snap_new (pid, &pidfd_owned, &local_error);

  if (!app_info && !g_error_matches (local_error, XDP_APP_INFO_ERROR,
                                     XDP_APP_INFO_ERROR_WRONG_APP_KIND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  g_clear_error (&local_error);

  if (app_info == NULL)
    app_info = xdp_app_info_host_new (pid, &pidfd_owned);

  g_assert (XDP_IS_APP_INFO (app_info));

  return g_steal_pointer (&app_info);
}

static XdpAppInfo *
xdp_app_info_new_for_invocation_sync (GDBusMethodInvocation  *invocation,
                                      GCancellable           *cancellable,
                                      GError                **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autofd int pidfd = -1;
  uint32_t pid;
  g_autoptr(GError) local_error = NULL;

  if (!xdp_connection_get_pidfd_sync (connection, sender,
                                      cancellable,
                                      &pidfd, &pid,
                                      error))
    return NULL;

  return xdp_app_info_new (pid, g_steal_fd (&pidfd), error);
}

static XdpAppInfo *
xdp_app_info_new_for_registered_sync (GDBusMethodInvocation  *invocation,
                                      const char             *app_id,
                                      GCancellable           *cancellable,
                                      GError                **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autofd int pidfd = -1;
  uint32_t pid;
  g_autoptr(GError) local_error = NULL;

  if (!xdp_connection_get_pidfd_sync (connection, sender,
                                      cancellable,
                                      &pidfd, &pid,
                                      error))
    return NULL;

  return xdp_app_info_host_new_registered (pid, g_steal_fd (&pidfd),
                                           app_id,
                                           error);
}

const char *
xdp_app_info_get_app_display_name (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  if (priv->gappinfo)
    return g_app_info_get_display_name (priv->gappinfo);

  if (g_strcmp0 (priv->id, "") != 0)
    return priv->id;

  return NULL;
}

const char *
xdp_app_info_get_engine_display_name (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  if (priv->engine && g_strcmp0 (priv->engine, "") != 0)
    return priv->engine;

  return g_type_name (G_OBJECT_TYPE (app_info));
}

gboolean
xdp_app_info_is_host (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, FALSE);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->engine == NULL;
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
xdp_app_info_get_engine (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv;

  g_return_val_if_fail (app_info != NULL, NULL);

  priv = xdp_app_info_get_instance_private (app_info);

  return priv->engine;
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

  return (priv->flags & XDP_APP_INFO_FLAG_HAS_NETWORK) != 0;
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

  if (!xdp_pidfd_get_pidns (priv->pidfd, &ns, error))
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
   * don't, such as socket:[27345] or anon_inode:[eventfd].
   * We don't support any of these.
   */
  if (path_buffer[0] != '/')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                   "Not a regular file or directory: %s", path_buffer);
      return NULL;
    }

  /* File descriptors to actually deleted files have " (deleted)"
   * appended to them. This also happens to some fake fd types
   * like shmem which are "/<name> (deleted)". All such
   * files are considered invalid. Unfortunately this also
   * matches files with filenames that actually end in " (deleted)",
   * but there is not much to do about this.
   */
  if (g_str_has_suffix (path_buffer, " (deleted)"))
    {
      const char *mountpoint = xdp_get_documents_mountpoint ();

      if (mountpoint != NULL && g_str_has_prefix (path_buffer, mountpoint))
        {
          /* Unfortunately our workaround for dcache purging triggers
           * o_path file descriptors on the fuse filesystem being
           * marked as deleted, so we have to allow these here and
           * rewrite them. This is safe, becase we will stat the file
           * and compare to make sure we end up on the right file.
           */
          path_buffer[symlink_size - strlen (" (deleted)")] = 0;
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

      if ((priv->flags & XDP_APP_INFO_FLAG_SUPPORTS_OPATH) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "App \"%s\" of type %s does not support O_PATH fd passing",
                       priv->id,
                       xdp_app_info_get_engine_display_name (app_info));
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

const GPtrArray *
xdp_app_info_get_usb_queries (XdpAppInfo *app_info)
{
  XdpAppInfoPrivate *priv = xdp_app_info_get_instance_private (app_info);

  if (!priv->id ||
      !XDP_APP_INFO_GET_CLASS (app_info)->get_usb_queries)
    {
      return NULL;
    }

  return XDP_APP_INFO_GET_CLASS (app_info)->get_usb_queries (app_info);
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

static gboolean
cache_has_app_info_by_sender (const char *sender)
{
  gboolean has_app_info = FALSE;

  G_LOCK (app_infos);
  if (app_info_by_unique_name)
    has_app_info = !!g_hash_table_lookup (app_info_by_unique_name, sender);
  G_UNLOCK (app_infos);

  return has_app_info;
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

void
xdp_app_info_delete_for_sender (const char *sender)
{
  G_LOCK (app_infos);
  if (app_info_by_unique_name)
    g_hash_table_remove (app_info_by_unique_name, sender);
  G_UNLOCK (app_infos);
}

XdpAppInfo *
xdp_invocation_ensure_app_info_sync (GDBusMethodInvocation  *invocation,
                                     GCancellable           *cancellable,
                                     GError                **error)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(XdpAppInfo) app_info = NULL;

  app_info = cache_lookup_app_info_by_sender (sender);
  if (app_info)
    return g_steal_pointer (&app_info);

  app_info = xdp_app_info_new_for_invocation_sync (invocation,
                                                   cancellable,
                                                   error);

  g_debug ("Adding %s app '%s'",
           xdp_app_info_get_engine_display_name (app_info),
           xdp_app_info_get_id (app_info));

  cache_insert_app_info (sender, app_info);

  return g_steal_pointer (&app_info);
}

XdpAppInfo *
xdp_invocation_register_host_app_info_sync (GDBusMethodInvocation  *invocation,
                                            const char             *app_id,
                                            GCancellable           *cancellable,
                                            GError                **error)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(XdpAppInfo) detected_app_info = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;

  if (cache_has_app_info_by_sender (sender))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Connection already associated with an application ID");
      return NULL;
    }

  detected_app_info = xdp_app_info_new_for_invocation_sync (invocation,
                                                            cancellable,
                                                            error);
  if (!detected_app_info)
    return NULL;

  if (!xdp_app_info_is_host (detected_app_info))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't manually register a %s application",
                   xdp_app_info_get_engine_display_name (detected_app_info));
      return NULL;
    }

  app_info = xdp_app_info_new_for_registered_sync (invocation,
                                                   app_id,
                                                   cancellable,
                                                   error);
  if (!app_info)
    return NULL;

  g_debug ("Adding registered host app '%s'", xdp_app_info_get_id (app_info));

  cache_insert_app_info (sender, app_info);

  return g_steal_pointer (&app_info);
}
