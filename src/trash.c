/*
 * Copyright © 2026 Red Hat, Inc
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
 */

#include "config.h"

#include "trash.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixmounts.h>
#include <libglnx.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-documents.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Trash Trash;
typedef struct _TrashClass TrashClass;

struct _Trash
{
  XdpDbusTrashSkeleton parent_instance;
};

struct _TrashClass
{
  XdpDbusTrashSkeletonClass parent_class;
};

GType trash_get_type (void) G_GNUC_CONST;
static void trash_iface_init (XdpDbusTrashIface *iface);

G_DEFINE_TYPE_WITH_CODE (Trash, trash, XDP_DBUS_TYPE_TRASH_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_TRASH,
                                                trash_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Trash, g_object_unref)

/* Check whether subsequently deleting the original file from the trash
 * (in the gvfsd-trash process) will succeed. If we think it won’t, return
 * an error, as the trash spec says trashing should not be allowed.
 * https://specifications.freedesktop.org/trash-spec/latest/#implementation-notes
 *
 * Check ownership to see if we can delete. gvfsd will automatically chmod
 * a file to allow it to be deleted, so checking the permissions bitfield isn’t
 * relevant.
 */
static gboolean
check_removing_recursively (int            fd,
                            gboolean       user_owned,
                            uid_t          uid,
                            GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = {0};

  if (!glnx_dirfd_iterator_init_at (fd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter,
                                                       &dent,
                                                       NULL,
                                                       error))
        return FALSE;

      if (dent == NULL)
        return TRUE;

      if (!user_owned)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "Unable to trash child file %s", dent->d_name);
          return FALSE;
        }

      if (dent->d_type == DT_DIR)
        {
          g_autofd int child = -1;
          struct glnx_statx stx;

          child = glnx_chase_and_statxat (fd, dent->d_name,
                                          GLNX_CHASE_NOFOLLOW |
                                          GLNX_CHASE_MUST_BE_DIRECTORY,
                                          GLNX_STATX_UID,
                                          &stx,
                                          error);
          if (child < 0)
            return FALSE;

          if (!check_removing_recursively (child,
                                           uid == stx.stx_uid,
                                           uid,
                                           error))
            return FALSE;
        }
    }
}

static gboolean
ignore_trash_mount_fd (XdpAppInfo *app_info,
                       int         mnt_fd)
{
  g_autofree char *mnt_path = NULL;
  g_autoptr(GUnixMountEntry) mount = NULL;
  const char *mount_options = NULL;
  g_autoptr(GError) local_error = NULL;

  /* If we run the tests, the test directory will be on a tmpfs mount or some
   * other mount that we would ignore in production, but we need to not ignore
   * it to test it properly. */
  if (g_getenv ("XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND") != NULL)
    return FALSE;

  mnt_path = xdp_app_info_get_path_for_fd (app_info, mnt_fd,
                                           0, NULL, NULL,
                                           &local_error);
  if (!mnt_path)
    {
      g_debug ("Ignoring the trash dir, because the mount fd can't be "
               "converted to a path: %s",
               local_error->message);
      return TRUE;
    }

#if GLIB_CHECK_VERSION(2,84,0)
  mount = g_unix_mount_entry_at (mnt_path, NULL);
#else
  mount = g_unix_mount_at (mnt_path, NULL);
#endif

  if (!mount)
    {
      g_debug ("Ignoring the trash dir, because not mount entry for the mount "
               "directory could be found");
      return TRUE;
    }

#if GLIB_CHECK_VERSION(2,84,0)
  mount_options = g_unix_mount_entry_get_options (mount);
#else
  mount_options = g_unix_mount_get_options (mount);
#endif

  if (mount_options == NULL)
    {
      g_autoptr(GUnixMountPoint) mount_point = NULL;

      mount_point = g_unix_mount_point_at (mnt_path,  NULL);
      if (mount_point != NULL)
        mount_options = g_unix_mount_point_get_options (mount_point);
    }

  if (mount_options != NULL)
    {
      if (strstr (mount_options, "x-gvfs-trash") != NULL)
        return FALSE;

      if (strstr (mount_options, "x-gvfs-notrash") != NULL)
        {
          g_debug ("Ignoring the trash dir, because mount options include x-gvfs-notrash");
          return TRUE;
        }
    }

  {
    gboolean is_internal;

#if GLIB_CHECK_VERSION(2,84,0)
    is_internal = g_unix_mount_entry_is_system_internal (mount);
#else
    is_internal = g_unix_mount_is_system_internal (mount);
#endif

    if (is_internal)
      g_debug ("Ignoring the trash dir, because mount is internal");

    return is_internal;
  }
}


static int
get_child_mkdir_p_0700 (int                 fd,
                        const char         *path,
                        struct glnx_statx  *stx,
                        GError            **error)
{
  g_autofd int child = -1;

  /* Would be nice to use glnx_chase only, but we have to mkdir with 0700 perms
   * and that's not supported right now. */

  if (!glnx_ensure_dir (fd, path, 0700, error))
    return -1;

  child = glnx_chase_and_statxat (fd, path,
                                  GLNX_CHASE_NOFOLLOW |
                                  GLNX_CHASE_MUST_BE_DIRECTORY,
                                  GLNX_STATX_MODE | GLNX_STATX_UID,
                                  stx,
                                  error);

  if ((stx->stx_mode & ~S_IFMT) != 0700)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "Directory already exists with bad permissions");
      return -1;
    }

  return g_steal_fd (&child);
}

static gboolean
stat_mnt (int                 fd,
          struct glnx_statx  *stx,
          GError            **error)
{
  if (!glnx_statx (fd, "",
                   AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                   GLNX_STATX_TYPE | GLNX_STATX_INO |
                   GLNX_STATX_MNT_ID | GLNX_STATX_MNT_ID_UNIQUE,
                   stx,
                   error))
    return FALSE;

  if ((stx->stx_mask & (GLNX_STATX_TYPE | GLNX_STATX_INO)) !=
        (GLNX_STATX_TYPE | GLNX_STATX_INO) ||
      (stx->stx_mask & (GLNX_STATX_MNT_ID | GLNX_STATX_MNT_ID_UNIQUE)) == 0)
    {
      g_set_error_literal (error, G_IO_ERROR,
                           g_io_error_from_errno (EXDEV),
                           g_strerror (EXDEV));
      return FALSE;
    }

  return TRUE;
}

static gboolean
get_mnt (int        fd,
         int        parent_fd,
         int       *mnt_fd_out,
         uint64_t  *mnt_id_out,
         GError   **error)
{
  struct glnx_statx target_stx;
  g_autofd int mnt = -1;
  struct glnx_statx stx = {0};
  g_autofd int next_mnt = -1;
  struct glnx_statx next_stx;

  if (!stat_mnt (fd, &target_stx, error))
    return FALSE;

  next_mnt = glnx_chaseat (parent_fd, ".", GLNX_CHASE_DEFAULT, error);
  if (next_mnt < 0)
    return FALSE;

  if (!stat_mnt (next_mnt, &next_stx, error))
    return FALSE;

  while (TRUE)
    {
      /* If the dir up is on a different mount, we found the mount point */
      if (target_stx.stx_mnt_id != next_stx.stx_mnt_id)
        break;

      /* If we hit root, we end up at the same ino+stx_mnt_id, and / is our
       * mount point */
      if (stx.stx_mask != 0 &&
          stx.stx_ino == next_stx.stx_ino &&
          stx.stx_mnt_id == next_stx.stx_mnt_id)
        break;

      g_clear_fd (&mnt, NULL);
      mnt = g_steal_fd (&next_mnt);
      stx = next_stx;

      next_mnt = glnx_chaseat (mnt, "..", GLNX_CHASE_DEFAULT, error);
      if (next_mnt < 0)
        return FALSE;

      if (!stat_mnt (next_mnt, &next_stx, error))
        return FALSE;
    }

  if (mnt < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVAL,
                           "No parent mount point");
      return FALSE;
    }

  if (mnt_fd_out)
    *mnt_fd_out = g_steal_fd (&mnt);

  if (mnt_id_out)
    *mnt_id_out = stx.stx_mnt_id;

  return TRUE;
}

static gboolean
get_trash_dir_home (uint64_t   mnt_id,
                    int       *trash_dir_out,
                    GError   **error)
{
  g_autofree char *trash_path = NULL;
  g_autofd int trash = -1;
  struct glnx_statx stx;

  /* We use paths here because this must not be in control of an attacker anyway */
  trash_path = g_build_filename (g_get_user_data_dir (), "Trash", NULL);
  if (g_mkdir_with_parents (trash_path, 0700) < 0)
    {
      int errsv = errno;
      g_autofree char *display_name = NULL;

      g_set_error (error, G_IO_ERROR,
                   g_io_error_from_errno (errsv),
                   "Unable to create trash directory %s: %s",
                   display_name, g_strerror (errsv));
      return FALSE;
    }

  trash = glnx_chaseat (AT_FDCWD, trash_path, GLNX_CHASE_DEFAULT, error);
  if (trash < 0)
    return FALSE;

  if (!stat_mnt (trash, &stx, error))
    return FALSE;

  /* The home trash is on the same mount as the target, so we use it! */
  if (stx.stx_mnt_id != mnt_id)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVAL,
                           "Target file is not on the same mount as home");
      return FALSE;
    }

  if (trash_dir_out)
    *trash_dir_out = g_steal_fd (&trash);

  return TRUE;
}

static gboolean
get_trash_dir_topdir_sticky (int      mnt_fd,
                             int     *trash_fd_out,
                             GError **error)
{
  g_autofd int trash = -1;
  g_autofd int trash_user = -1;
  struct glnx_statx stx;
  uid_t uid;
  g_autofree char *uid_str = NULL;

  trash = glnx_chase_and_statxat (mnt_fd, ".Trash",
                                  GLNX_CHASE_NOFOLLOW,
                                  GLNX_STATX_MODE | GLNX_STATX_TYPE,
                                  &stx, error);
  if (trash < 0)
    return FALSE;

  if (!S_ISDIR (stx.stx_mode) || (stx.stx_mode & S_ISVTX) == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           ".Trash is not a directory or is missing the sticky bit");
      return FALSE;
    }

  uid = geteuid ();
  uid_str = g_strdup_printf ("%lu", (unsigned long) uid);

  trash_user = get_child_mkdir_p_0700 (trash, uid_str, &stx, error);
  if (trash_user < 0)
    return FALSE;

  if (stx.stx_uid != uid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "User dir not owned by the user");
      return FALSE;
    }

  if (trash_fd_out)
    *trash_fd_out = g_steal_fd (&trash_user);

  return TRUE;
}

static gboolean
get_trash_dir_topdir_user (int      mnt_fd,
                           int     *trash_fd_out,
                           GError **error)
{
  g_autofd int trash = -1;
  g_autofd int trash_user = -1;
  struct glnx_statx stx;

  uid_t uid;
  g_autofree char *trash_name = NULL;

  uid = geteuid ();
  trash_name = g_strdup_printf (".Trash-%lu", (unsigned long) uid);

  trash_user = get_child_mkdir_p_0700 (mnt_fd, trash_name, &stx, error);
  if (trash_user < 0)
    return FALSE;

  if (stx.stx_uid != uid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "User dir not owned by the user");
      return FALSE;
    }

  if (trash_fd_out)
    *trash_fd_out = g_steal_fd (&trash_user);

  return TRUE;
}

gboolean
get_trash_dir (XdpAppInfo  *app_info,
               int          target_fd,
               int          parent_fd,
               int         *trash_fd_out,
               int         *topdir_fd_out,
               GError     **error)
{
  g_autofd int mnt_fd = -1;
  uint64_t mnt_id;
  g_autofd int trash_dir = -1;
  g_autoptr(GError) local_error = NULL;

  if (trash_fd_out)
    *trash_fd_out = -1;
  if (topdir_fd_out)
    *topdir_fd_out = -1;

  if (!get_mnt (target_fd, parent_fd, &mnt_fd, &mnt_id, error))
    return FALSE;

  /* First choice is always the home trash. */
  if (get_trash_dir_home (mnt_id, trash_fd_out, &local_error))
    return TRUE;

  if (ignore_trash_mount_fd (app_info, mnt_fd))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVAL,
                           "No suitable trash directory found");
      return FALSE;
    }

  g_debug ("Skipping home dir trash: %s", local_error->message);
  g_clear_error (&local_error);

  if (get_trash_dir_topdir_sticky (mnt_fd, trash_fd_out, &local_error))
    {
      if (topdir_fd_out)
        *topdir_fd_out = g_steal_fd (&mnt_fd);

      return TRUE;
    }

  g_debug ("Skipping sticky topdir trash: %s", local_error->message);
  g_clear_error (&local_error);

  if (get_trash_dir_topdir_user (mnt_fd, trash_fd_out, &local_error))
    {
      if (topdir_fd_out)
        *topdir_fd_out = g_steal_fd (&mnt_fd);

      return TRUE;
    }

  g_debug ("Skipping user topdir trash: %s", local_error->message);
  g_clear_error (&local_error);

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVAL,
                       "No suitable trash directory found");
  return FALSE;
}

static char *
get_unique_trash_name (const char *basename,
                       int         id)
{
  const char *dot;

  if (id == 1)
    return g_strdup (basename);

  dot = strchr (basename, '.');
  if (dot)
    return g_strdup_printf ("%.*s.%d%s", (int)(dot - basename), basename, id, dot);
  else
    return g_strdup_printf ("%s.%d", basename, id);
}

static int
open_parent (int          fd,
             const char  *path,
             GError     **error)
{
  g_autofree char *parent_path = NULL;
  g_autofree char *base_name = NULL;
  g_autofd int parent_fd = -1;
  g_autofd int verify_fd = -1;
  struct glnx_statx stx;
  struct glnx_statx verify_stx;

  parent_path = g_path_get_dirname (path);
  parent_fd = glnx_chaseat (AT_FDCWD, parent_path,
                            GLNX_CHASE_NOFOLLOW | GLNX_CHASE_MUST_BE_DIRECTORY,
                            error);
  if (parent_fd < 0)
    return -1;

  base_name = g_path_get_basename (path);
  verify_fd = glnx_chaseat (parent_fd, base_name,
                            GLNX_CHASE_NOFOLLOW,
                            error);
  if (verify_fd < 0)
    return -1;

  if (!stat_mnt (fd, &stx, error))
    return -1;

  if (!stat_mnt (verify_fd, &verify_stx, error))
    return -1;

  if (stx.stx_ino != verify_stx.stx_ino ||
      stx.stx_mnt_id != verify_stx.stx_mnt_id)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Failed getting the parent fd");
      return -1;
    }

  return g_steal_fd (&parent_fd);
}

static gboolean
trash_file (int          target_fd_in,
            XdpAppInfo  *app_info,
            GError     **error)
{
  g_autofd int target_fd = -1;
  g_autofree char *target_path = NULL;
  gboolean writable;
  g_autofd int parent_fd = -1;
  g_autofd int trash_fd = -1;
  g_autofree char *restore_path = NULL;
  g_autofree char *restore_data = NULL;

  target_path = xdp_app_info_get_path_for_fd (app_info,
                                              target_fd_in,
                                              0, NULL,
                                              &writable,
                                              error);
  if (!target_path)
    return FALSE;

  g_debug ("Trying to trash file at '%s' on host", target_path);

  if (!writable)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "File descriptor is not opened for writing");
      return FALSE;
    }

  /* target_fd_in might be in the mount namespace of the caller and thus on a
   * different mount than we expect in the host mount namespace. Let's reopen
   * it and verify that we opened the right file. */
  {
    struct glnx_statx stx_in;
    struct glnx_statx stx;

    if (!glnx_statx (target_fd_in, "",
                     AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                     GLNX_STATX_INO,
                     &stx_in,
                     error))
      return FALSE;

    target_fd = glnx_chase_and_statxat (AT_FDCWD, target_path,
                                        GLNX_CHASE_NOFOLLOW,
                                        GLNX_STATX_INO,
                                        &stx,
                                        error);
    if (target_fd < 0)
      return FALSE;

    if (!(stx.stx_mask & GLNX_STATX_INO) || !(stx_in.stx_mask & GLNX_STATX_INO) ||
        stx.stx_ino != stx_in.stx_ino ||
        stx.stx_dev_major != stx_in.stx_dev_major ||
        stx.stx_dev_minor != stx_in.stx_dev_minor)
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Cannot determine path on the host");
        return FALSE;
      }
  }

  parent_fd = open_parent (target_fd, target_path, error);
  if (parent_fd < 0)
    return FALSE;

  {
    g_autofd int topdir_fd = -1;
    g_autofree char *topdir_path = NULL;

    if (!get_trash_dir (app_info, target_fd, parent_fd, &trash_fd, &topdir_fd, error))
      return FALSE;

    /* Only the homedir doesn't have a topdir */
    if (topdir_fd >= 0)
      {
        const char *path;

        topdir_path = xdp_app_info_get_path_for_fd (app_info, topdir_fd, 0, NULL, NULL, error);
        if (!topdir_path)
          return FALSE;

        if (!g_str_has_prefix (target_path, topdir_path))
          {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "Cannot determine relative path from trash to file");
            return FALSE;
          }

        path = target_path + strlen (topdir_path);
        while (path[0] == '/')
          path++;

        restore_path = g_strdup (path);
      }
    else
      {
        restore_path = g_strdup (target_path);
      }
  }

  /* We can verify as much as we want here, the problem is going to be
   * restoring: if restoring follows symlinks, they end up in attacker control
   * and can override any file of the same user (for example your
   * `~/.ssh/authorized_keys`).
   */

  {
    g_autofree char *restore_path_escaped = NULL;
    g_autoptr(GDateTime) now = NULL;
    g_autofree char *delete_time = NULL;

    restore_path_escaped = g_uri_escape_string (restore_path, "/", FALSE);

    now = g_date_time_new_now_local ();
    if (now != NULL)
      delete_time = g_date_time_format (now, "%Y-%m-%dT%H:%M:%S");
    else
      delete_time = g_strdup ("9999-12-31T23:59:59");

    restore_data = g_strdup_printf ("[Trash Info]\nPath=%s\nDeletionDate=%s\n",
                                    restore_path_escaped,
                                    delete_time);
  }

  {
    struct glnx_statx stx;

    if (!glnx_statx (target_fd, "",
                     AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                     GLNX_STATX_MODE | GLNX_STATX_UID,
                     &stx,
                     error))
      return FALSE;

    if (S_ISDIR (stx.stx_mode))
      {
        uid_t uid = geteuid ();

        if (stx.stx_uid == uid &&
            !check_removing_recursively (target_fd, TRUE, uid, error))
          return FALSE;
      }
  }

  {
    g_autofd int info_fd = -1;
    g_autofd int files_fd = -1;
    g_autofree char *basename = NULL;
    char *basename_candidate = NULL;
    g_autofree char *trashname = NULL;
    g_autofd int info_file_fd = -1;
    struct glnx_statx stx;
    size_t i = 0;

    info_fd = get_child_mkdir_p_0700 (trash_fd, "info", &stx, error);
    if (info_fd < 0)
      return FALSE;

    files_fd = get_child_mkdir_p_0700 (trash_fd, "files", &stx, error);
    if (files_fd < 0)
      return FALSE;

    basename = g_path_get_basename (restore_path);
    basename_candidate = basename;

    while (TRUE)
      {
        g_autofree char *local_trashname = NULL;
        g_autofree char *infoname = NULL;
        g_autofd int local_info_file_fd = -1;

        local_trashname = get_unique_trash_name (basename_candidate, i++);
        infoname = g_strconcat (local_trashname, ".trashinfo", NULL);

        local_info_file_fd = openat (info_fd, infoname,
                                     O_CREAT | O_EXCL | O_WRONLY |
                                     O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC,
                                     0700);

        if (local_info_file_fd >= 0)
          {
            if (glnx_loop_write (local_info_file_fd,
                                 restore_data,
                                 strlen (restore_data)) < 0)
              return glnx_throw_errno (error);

            trashname = g_steal_pointer (&local_trashname);
            info_file_fd = g_steal_fd (&local_info_file_fd);
            break;
          }

        if (errno == ENAMETOOLONG)
          {
            size_t len = strlen (basename_candidate);

            if (len <= strlen (".trashinfo"))
              return glnx_throw_errno (error); /* fail with ENAMETOOLONG */

            len -= strlen (".trashinfo");
            memmove (basename_candidate,
                     basename_candidate + strlen (".trashinfo"),
                     len);
            basename_candidate[len] = '\0';
            i = 1;
            continue;
          }

        if (errno != EEXIST)
          return glnx_throw_errno (error);
      }

    g_clear_pointer (&basename, g_free);
    basename = g_path_get_basename (restore_path);

    /* This is inherently racy. We can do our best and
     * statx(parent_fd, basename) again and see that the inode is still the
     * same, but then we still pass in the path. */
    if (glnx_renameat2_noreplace (parent_fd, basename, files_fd, trashname) < 0)
      {
        int errsv = errno;

        unlinkat (info_fd, trashname, 0);

        errno = errsv;
        return glnx_throw_errno (error);
      }
  }

  return TRUE;
}

static gboolean
handle_trash_file (XdpDbusTrash          *object,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList           *fd_list,
                   GVariant              *arg_fd)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autofd int fd = -1;
  guint result;
  g_autoptr(GError) error = NULL;

  g_debug ("Handling TrashFile");

  fd = xdp_get_portal_call_fd (fd_list, g_variant_get_handle (arg_fd), &error);
  if (fd < 0)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!trash_file (fd, app_info, &error))
    {
      g_debug ("Failed trashing file: %s", error->message);
      result = 0;
    }
  else
    {
      result = 1;
    }

  xdp_dbus_trash_complete_trash_file (object, g_steal_pointer (&invocation),
                                      NULL, result);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
trash_iface_init (XdpDbusTrashIface *iface)
{
  iface->handle_trash_file = handle_trash_file;
}

static void
trash_init (Trash *trash)
{
}

static void
trash_class_init (TrashClass *klass)
{
}

void
init_trash (XdpContext *context)
{
  g_autoptr(Trash) trash = NULL;

  trash = g_object_new (trash_get_type (), NULL);
  xdp_dbus_trash_set_version (XDP_DBUS_TRASH (trash), 1);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&trash)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
