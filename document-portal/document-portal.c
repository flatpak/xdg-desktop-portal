/*
 * Copyright © 2018 Red Hat, Inc
 * Copyright © 2023 GNOME Foundation Inc.
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
 *       Hubert Figuière <hub@figuiere.net>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "glib-backports.h"
#include "document-portal-dbus.h"
#include "document-store.h"
#include "src/xdp-utils.h"
#include "permission-db.h"
#include "permission-store-dbus.h"
#include "document-portal-fuse.h"
#include "file-transfer.h"
#include "document-portal.h"

#define TABLE_NAME "documents"

typedef struct
{
  char                  *doc_id;
  int                    fd;
  char                  *owner;
  guint                  flags;

  GDBusMethodInvocation *finish_invocation;
} XdpDocUpdate;


static GMainLoop *loop = NULL;
static PermissionDb *db = NULL;
static XdgPermissionStore *permission_store;
static int final_exit_status = 0;
static GError *exit_error = NULL;
static dev_t fuse_dev = 0;
static GQueue get_mount_point_invocations = G_QUEUE_INIT;
static XdpDbusDocuments *dbus_api;

G_LOCK_DEFINE (db);

char **
xdp_list_apps (void)
{
  XDP_AUTOLOCK (db);
  return permission_db_list_apps (db);
}

char **
xdp_list_docs (void)
{
  XDP_AUTOLOCK (db);
  return permission_db_list_ids (db);
}

PermissionDbEntry *
xdp_lookup_doc (const char *doc_id)
{
  XDP_AUTOLOCK (db);
  return permission_db_lookup (db, doc_id);
}

static gboolean
persist_entry (PermissionDbEntry *entry)
{
  guint32 flags = document_entry_get_flags (entry);

  return (flags & DOCUMENT_ENTRY_FLAG_TRANSIENT) == 0;
}

static void
do_set_permissions (PermissionDbEntry    *entry,
                    const char        *doc_id,
                    const char        *app_id,
                    DocumentPermissionFlags perms)
{
  g_autofree const char **perms_s = xdg_unparse_permissions (perms);

  g_autoptr(PermissionDbEntry) new_entry = NULL;

  g_debug ("set_permissions %s %s %x", doc_id, app_id, perms);

  new_entry = permission_db_entry_set_app_permissions (entry, app_id, perms_s);
  permission_db_set_entry (db, doc_id, new_entry);

  if (persist_entry (new_entry))
    {
      xdg_permission_store_call_set_permission (permission_store,
                                                TABLE_NAME,
                                                FALSE,
                                                doc_id,
                                                app_id,
                                                perms_s,
                                                NULL,
                                                NULL, NULL);
    }
}

static void
portal_grant_permissions (GDBusMethodInvocation *invocation,
                          GVariant              *parameters,
                          XdpAppInfo            *app_info)
{
  const char *target_app_id;
  const char *id;
  g_autofree const char **permissions = NULL;
  DocumentPermissionFlags perms;
  GError *error = NULL;

  g_autoptr(PermissionDbEntry) entry = NULL;

  g_variant_get (parameters, "(&s&s^a&s)", &id, &target_app_id, &permissions);

  {
    XDP_AUTOLOCK (db);

    entry = permission_db_lookup (db, id);
    if (entry == NULL)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                               "No such document: %s", id);
        return;
      }

    if (!xdp_is_valid_app_id (target_app_id))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "'%s' is not a valid app name", target_app_id);
        return;
      }

    perms = xdp_parse_permissions (permissions, &error);
    if (error)
      {
        g_dbus_method_invocation_take_error (invocation, error);
        return;
      }

    /* Must have grant-permissions and all the newly granted permissions */
    if (!document_entry_has_permissions (entry, app_info,
                                    DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS | perms))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    do_set_permissions (entry, id, target_app_id,
                        perms | document_entry_get_permissions_by_app_id (entry, target_app_id));
  }

  /* Invalidate with lock dropped to avoid deadlock */
  xdp_fuse_invalidate_doc_app (id, target_app_id);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
portal_revoke_permissions (GDBusMethodInvocation *invocation,
                           GVariant              *parameters,
                           XdpAppInfo            *app_info)
{
  const char *app_id = xdp_app_info_get_id (app_info);
  const char *target_app_id;
  const char *id;
  g_autofree const char **permissions = NULL;
  GError *error = NULL;

  g_autoptr(PermissionDbEntry) entry = NULL;
  DocumentPermissionFlags perms;

  g_variant_get (parameters, "(&s&s^a&s)", &id, &target_app_id, &permissions);

  {
    XDP_AUTOLOCK (db);

    entry = permission_db_lookup (db, id);
    if (entry == NULL)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                               "No such document: %s", id);
        return;
      }

    if (!xdp_is_valid_app_id (target_app_id))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "'%s' is not a valid app name", target_app_id);
        return;
      }

    perms = xdp_parse_permissions (permissions, &error);
    if (error)
      {
        g_dbus_method_invocation_take_error (invocation, error);
        return;
      }

    /* Must have grant-permissions, or be itself */
    if (!document_entry_has_permissions (entry, app_info,
                                    DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS) ||
        strcmp (app_id, target_app_id) == 0)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    do_set_permissions (entry, id, target_app_id,
                        ~perms & document_entry_get_permissions_by_app_id (entry, target_app_id));
  }

  /* Invalidate with lock dropped to avoid deadlock */
  xdp_fuse_invalidate_doc_app (id, target_app_id);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
portal_delete (GDBusMethodInvocation *invocation,
               GVariant              *parameters,
               XdpAppInfo            *app_info)
{
  const char *id;
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autofree const char **old_apps = NULL;
  int i;

  g_variant_get (parameters, "(&s)", &id);

  g_debug ("portal_delete %s", id);

  {
    XDP_AUTOLOCK (db);

    entry = permission_db_lookup (db, id);
    if (entry == NULL)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                               "No such document: %s", id);
        return;
      }

    if (!document_entry_has_permissions (entry, app_info, DOCUMENT_PERMISSION_FLAGS_DELETE))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    g_debug ("delete %s", id);

    permission_db_set_entry (db, id, NULL);

    if (persist_entry (entry))
      xdg_permission_store_call_delete (permission_store, TABLE_NAME,
                                        id, NULL, NULL, NULL);
  }

  /* All i/o is done now, so drop the lock so we can invalidate the fuse caches */
  old_apps = permission_db_entry_list_apps (entry);
  for (i = 0; old_apps[i] != NULL; i++)
    xdp_fuse_invalidate_doc_app (id, old_apps[i]);
  xdp_fuse_invalidate_doc_app (id, NULL);

  /* Now fuse view is up-to-date, so we can return the call */
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static char *
do_create_doc (struct stat *parent_st_buf, const char *path, gboolean reuse_existing, gboolean persistent, gboolean directory)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_auto(GStrv) ids = NULL;
  char *id = NULL;
  guint32 flags = 0;

  g_debug ("Creating document at path '%s', reuse_existing: %d, persistent: %d, directory: %d", path, reuse_existing, persistent, directory);

  if (!reuse_existing)
    flags |= DOCUMENT_ENTRY_FLAG_UNIQUE;
  if (!persistent)
    flags |= DOCUMENT_ENTRY_FLAG_TRANSIENT;
  if (directory)
    flags |= DOCUMENT_ENTRY_FLAG_DIRECTORY;
  data =
    g_variant_ref_sink (g_variant_new ("(^ayttu)",
                                       path,
                                       (guint64) parent_st_buf->st_dev,
                                       (guint64) parent_st_buf->st_ino,
                                       flags));

  if (reuse_existing)
    {
      ids = permission_db_list_ids_by_value (db, data);

      if (ids[0] != NULL)
        return g_strdup (ids[0]);  /* Reuse pre-existing entry with same path */
    }

  while (TRUE)
    {
      g_autoptr(PermissionDbEntry) existing = NULL;

      g_clear_pointer (&id, g_free);
      id = xdp_name_from_id ((guint32) g_random_int ());
      existing = permission_db_lookup (db, id);
      if (existing == NULL)
        break;
    }

  g_debug ("create_doc %s", id);

  entry = permission_db_entry_new (data);
  permission_db_set_entry (db, id, entry);

  if (persistent)
    {
      xdg_permission_store_call_set (permission_store,
                                     TABLE_NAME,
                                     TRUE,
                                     id,
                                     g_variant_new_array (G_VARIANT_TYPE ("{sas}"), NULL, 0),
                                     g_variant_new_variant (data),
                                     NULL, NULL, NULL);
    }

  return id;
}

gboolean
validate_fd (int fd,
             XdpAppInfo *app_info,
             ValidateFdType ensure_type,
             struct stat *st_buf,
             struct stat *real_dir_st_buf,
             char **path_out,
             gboolean *writable_out,
             GError **error)
{
  g_autofree char *path = NULL;
  g_autofree char *dirname = NULL;
  g_autofree char *name = NULL;
  xdp_autofd int dir_fd = -1;
  struct stat real_st_buf;
  g_autoptr(GError) local_error = NULL;

  path = xdp_app_info_get_path_for_fd (app_info, fd, 0, st_buf, writable_out, &local_error);

  if (path == NULL)
    {
      g_debug ("Invalid fd passed: %s", local_error->message);
      goto errout;
    }

  if ((ensure_type == VALIDATE_FD_FILE_TYPE_REGULAR || ensure_type == VALIDATE_FD_FILE_TYPE_ANY) && S_ISREG (st_buf->st_mode))
    {
      /* We open the parent directory and do the stat in that, so that we have
       * trustworthy parent dev/ino + filename for later verification. Otherwise the caller
       * could later replace a parent with a symlink and make us read some other file.
       */
      dirname = g_path_get_dirname (path);
      name = g_path_get_basename (path);
    }
  else if ((ensure_type == VALIDATE_FD_FILE_TYPE_DIR || ensure_type == VALIDATE_FD_FILE_TYPE_ANY)  && S_ISDIR (st_buf->st_mode))
    {
      /* For dirs, we keep the dev/ino of the directory itself */
      dirname = g_strdup (path);
    }
  else
    goto errout;

  dir_fd = open (dirname, O_CLOEXEC | O_PATH);
  if (dir_fd < 0 || fstat (dir_fd, real_dir_st_buf) != 0)
    goto errout;

  if (name != NULL)
    {
      if (fstatat (dir_fd, name, &real_st_buf, AT_SYMLINK_NOFOLLOW) < 0 ||
          st_buf->st_dev != real_st_buf.st_dev ||
          st_buf->st_ino != real_st_buf.st_ino)
        goto errout;
    }
  else if (st_buf->st_dev != real_dir_st_buf->st_dev ||
           st_buf->st_ino != real_dir_st_buf->st_ino)
    goto errout;


  if (path_out)
    *path_out = g_steal_pointer (&path);

  return TRUE;

 errout:
  /* Don't leak any info about real file path existence, etc */
  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "Invalid fd passed");
  return FALSE;
}

static char *
verify_existing_document (struct stat *st_buf,
                          gboolean     reuse_existing,
                          gboolean     directory,
                          XdpAppInfo  *app_info,
                          gboolean     allow_write,
                          char       **real_path_out)
{
  g_autoptr(PermissionDbEntry) old_entry = NULL;
  g_autofree char *id = NULL;

  g_assert (st_buf->st_dev == fuse_dev);

  /* The passed in fd is on the fuse filesystem itself */
  id = xdp_fuse_lookup_id_for_inode (st_buf->st_ino, directory, real_path_out);
  g_debug ("path on fuse, id %s", id);
  if (id == NULL)
    return NULL;

  /* Don't lock the db before doing the fuse call above, because it takes takes a lock
     that can block something calling back, causing a deadlock on the db lock */
  XDP_AUTOLOCK (db);

  /* If the entry doesn't exist anymore, fail.  Also fail if not
   * reuse_existing, because otherwise the user could use this to
   * get a copy with permissions and thus escape later permission
   * revocations
   */
  old_entry = permission_db_lookup (db, id);
  if (old_entry == NULL || !reuse_existing)
    return NULL;

  /* Don't allow re-exposing non-writable document as writable */
  if (allow_write &&
      !document_entry_has_permissions (old_entry, app_info, DOCUMENT_PERMISSION_FLAGS_WRITE))
    return NULL;

  return g_steal_pointer (&id);
}

static void
portal_add (GDBusMethodInvocation *invocation,
            GVariant              *parameters,
            XdpAppInfo            *app_info)
{
  int fd_id;
  gboolean reuse_existing, persistent;
  DocumentAddFullFlags flags = 0;
  GDBusMessage *message;
  GUnixFDList *fd_list;
  GError *error = NULL;
  g_auto(GStrv) ids = NULL;

  g_variant_get (parameters, "(hbb)", &fd_id, &reuse_existing, &persistent);

  if (reuse_existing)
    flags |= DOCUMENT_ADD_FLAGS_REUSE_EXISTING;
  if (persistent)
    flags |= DOCUMENT_ADD_FLAGS_PERSISTENT;

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  if (fd_list != NULL)
    {
      int fds_len;
      const int *fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (fd_id < fds_len)
        {
          int fd = fds[fd_id];

          ids = document_add_full (&fd, NULL, NULL, &flags, 1, app_info, "", 0, &error);
        }
    }

  if (ids == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", ids[0]));
}

static char *
get_output (GError     **error,
            const char  *argv0,
            ...)
{
  gboolean res;
  g_autofree char *output = NULL;
  va_list ap;

  va_start (ap, argv0);
  res = xdp_spawn (NULL, &output, 0, error, argv0, ap);
  va_end (ap);

  if (res)
    {
      g_strchomp (output);
      return g_steal_pointer (&output);
    }
  return NULL;
}

/* out =>
     0 == hidden
     1 == read-only
     2 == read-write
*/
static void
metadata_check_file_access (const char *keyfile_path,
                            int *allow_host_out,
                            int *allow_home_out)
{
  g_autoptr(GKeyFile) keyfile = NULL;
  g_auto(GStrv) fss = NULL;

  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, keyfile_path, G_KEY_FILE_NONE, NULL))
    return;

  fss = g_key_file_get_string_list (keyfile, "Context",  "filesystems", NULL, NULL);
  if (fss)
    {
      int i;
      for (i = 0; fss[i] != NULL; i++)
        {
          const char *fs = fss[i];

          if (strcmp (fs, "!host") == 0)
            *allow_host_out = 0;
          if (strcmp (fs, "host:ro") == 0)
            *allow_host_out = 1;
          if (strcmp (fs, "host") == 0)
            *allow_host_out = 2;

          if (strcmp (fs, "!home") == 0)
            *allow_home_out = 0;
          if (strcmp (fs, "home:ro") == 0)
            *allow_home_out = 1;
          if (strcmp (fs, "home") == 0)
            *allow_home_out = 2;
        }
    }
}

/* This is a simplified version that only looks at filesystem=host and
 * filesystem=home, as such it should not cause false positives, but
 * be may create a document for files that the app should have access
 * to (e.g. when the app has a more strict access but the file is
 * still accessible) */
static gboolean
app_has_file_access_fallback (const char *target_app_id,
                              DocumentPermissionFlags target_perms,
                              const char *path)
{
  g_autofree char *user_metadata = NULL;
  g_autofree char *system_metadata = NULL;
  g_autofree char *user_override = NULL;
  g_autofree char *system_override = NULL;
  g_autofree char *user_global_override = NULL;
  g_autofree char *system_global_override = NULL;
  g_autofree char *homedir = NULL;
  g_autofree char *canonical_path = NULL;
  gboolean is_in_home = FALSE;
  g_autofree char *user_installation = g_build_filename (g_get_user_data_dir (), "flatpak", NULL);
  const char *system_installation = "/var/lib/flatpak";
  int allow_host = 0;
  int allow_home = 0;

  if (g_str_has_prefix (path, "/usr") || g_str_has_prefix (path, "/app") || g_str_has_prefix (path, "/tmp"))
    return FALSE;

  user_metadata = g_build_filename (user_installation, "app", target_app_id, "current/active/metadata", NULL);
  system_metadata = g_build_filename (system_installation, "app", target_app_id, "current/active/metadata", NULL);
  user_override = g_build_filename (user_installation, "overrides", target_app_id, NULL);
  system_override = g_build_filename (system_installation, "overrides", target_app_id, NULL);
  user_global_override = g_build_filename (user_installation, "overrides", "global", NULL);
  system_global_override = g_build_filename (system_installation, "overrides", "global", NULL);

  metadata_check_file_access (system_metadata, &allow_host, &allow_home);
  metadata_check_file_access (user_metadata, &allow_host, &allow_home);
  metadata_check_file_access (system_global_override, &allow_host, &allow_home);
  metadata_check_file_access (system_override, &allow_host, &allow_home);
  metadata_check_file_access (user_global_override, &allow_host, &allow_home);
  metadata_check_file_access (user_override, &allow_host, &allow_home);

  if (allow_host == 2 ||
      ((allow_host == 1) &&
       (target_perms & DOCUMENT_PERMISSION_FLAGS_WRITE) == 0))
    return TRUE;

  homedir = xdp_canonicalize_filename (g_get_home_dir ());
  canonical_path = xdp_canonicalize_filename (path);

  is_in_home = xdp_has_path_prefix (canonical_path, homedir);

  if (is_in_home &&
      ((allow_home == 2) ||
       (allow_home == 1 && (target_perms & DOCUMENT_PERMISSION_FLAGS_WRITE) == 0)))
    return TRUE;

  return FALSE;
}


static gboolean
app_has_file_access (const char *target_app_id,
                     DocumentPermissionFlags target_perms,
                     const char *path)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *res = NULL;
  g_autofree char *arg = NULL;

  if (target_app_id == NULL || target_app_id[0] == '\0')
    return FALSE;

  if (g_str_has_prefix (target_app_id, "snap."))
    {
      res = get_output (&error, "snap", "routine", "file-access",
                        target_app_id + strlen ("snap."), path, NULL);
    }
  else
    {
      /* First we try flatpak info --file-access=PATH APPID, which is supported on new versions */
      arg = g_strdup_printf ("--file-access=%s", path);
      res = get_output (&error, "flatpak", "info", arg, target_app_id, NULL);
    }

  if (res)
    {
      if (strcmp (res, "read-write") == 0)
        return TRUE;

      if (strcmp (res, "read-only") == 0 &&
          ((target_perms & DOCUMENT_PERMISSION_FLAGS_WRITE) == 0))
        return TRUE;

      return FALSE;
    }

  /* Secondly we fall back to a simple check that will not be perfect but should not
     cause false positives. */
  return app_has_file_access_fallback (target_app_id, target_perms, path);
}

static void
portal_add_full (GDBusMethodInvocation *invocation,
                 GVariant              *parameters,
                 XdpAppInfo            *app_info)
{
  g_autoptr(GVariant) array = NULL;
  guint32 flags;
  const char *target_app_id;
  g_autofree const char **permissions = NULL;
  DocumentPermissionFlags target_perms;
  gsize n_args;
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autofree int *fd = NULL;
  g_autofree DocumentAddFullFlags *documents_flags = NULL;
  g_auto(GStrv) ids = NULL;
  GError *error = NULL;
  GVariantBuilder builder;
  int fds_len;
  int i;
  const int *fds;

  g_variant_get (parameters, "(@ahu&s^a&s)",
                 &array, &flags, &target_app_id, &permissions);

  if ((flags & ~DOCUMENT_ADD_FLAGS_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid flags");
      return;
    }

  target_perms = xdp_parse_permissions (permissions, &error);
  if (error)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return;
    }

  n_args = g_variant_n_children (array);
  fd = g_new (int, n_args);
  documents_flags = g_new (DocumentAddFullFlags, n_args);
  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  if (fd_list == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "No fds passed");
      return;
    }

  fds_len = 0;
  fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
  for (i = 0; i < n_args; i++)
    {
      int fd_id;
      documents_flags[i] = flags;
      g_variant_get_child (array, i, "h", &fd_id);
      if (fd_id < fds_len)
        fd[i] = fds[fd_id];
      else
        fd[i] = -1;
    }

  ids = document_add_full (fd, NULL, NULL, documents_flags, n_args, app_info, target_app_id, target_perms, &error);

  if (ids == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "mountpoint",
                         g_variant_new_bytestring (xdp_fuse_get_mountpoint ()));

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(^as@a{sv})",
                                                        (char **)ids,
                                                        g_variant_builder_end (&builder)));
}

/*
 * if the fd array contains fds that were not opened by the client itself,
 * parent_dev and parent_ino must contain the st_dev/st_ino fields for the
 * parent directory to check for, to prevent symlink attacks.
 */
char **
document_add_full (int                      *fd,
                   int                      *parent_dev,
                   int                      *parent_ino,
                   DocumentAddFullFlags     *documents_flags,
                   int                       n_args,
                   XdpAppInfo               *app_info,
                   const char               *target_app_id,
                   DocumentPermissionFlags   target_perms,
                   GError                  **error)
{
  const char *app_id = xdp_app_info_get_id (app_info);
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_autofree struct stat *real_dir_st_bufs = NULL;
  struct stat st_buf;
  g_autofree gboolean *writable = NULL;
  int i;

  g_ptr_array_set_size (paths, n_args + 1);
  g_ptr_array_set_size (ids, n_args + 1);
  real_dir_st_bufs = g_new0 (struct stat, n_args);
  writable = g_new0 (gboolean, n_args);

  for (i = 0; i < n_args; i++)
    {
      DocumentAddFullFlags flags;
      g_autofree char *path = NULL;
      gboolean reuse_existing, allow_write, is_dir;

      flags = documents_flags[i];
      reuse_existing = (flags & DOCUMENT_ADD_FLAGS_REUSE_EXISTING) != 0;
      is_dir = (flags & DOCUMENT_ADD_FLAGS_DIRECTORY) != 0;
      allow_write = (target_perms & DOCUMENT_PERMISSION_FLAGS_WRITE) != 0;

      if (!validate_fd (fd[i], app_info, is_dir ? VALIDATE_FD_FILE_TYPE_DIR : VALIDATE_FD_FILE_TYPE_REGULAR, &st_buf, &real_dir_st_bufs[i], &path, &writable[i], error))
        return NULL;

      if (parent_dev != NULL && parent_ino != NULL)
        {
          if (real_dir_st_bufs[i].st_dev != parent_dev[i] ||
              real_dir_st_bufs[i].st_ino != parent_ino[i])
            {
              g_set_error (error,
                           XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                           "Invalid parent directory");
              return NULL;
            }
        }

      if (allow_write && !writable[i])
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                       "Not enough permissions");
          return NULL;
        }

      if (st_buf.st_dev == fuse_dev)
        {
          g_autofree char *real_path = NULL;
          g_autofree char *id = NULL;

          /* The passed in fd is on the fuse filesystem itself */
          id = verify_existing_document (&st_buf, reuse_existing, is_dir, app_info, allow_write, &real_path);
          if (id == NULL)
            {
              g_set_error (error,
                           XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "Invalid fd passed");
              return NULL;
            }

          /* Maybe this was a file on a directory document and we can expose the real path instead */
          if (real_path)
            {
              g_autofree char *dirname = NULL;

              g_free (path);
              path = g_steal_pointer (&real_path);
              /* Need to update real_dir_st_bufs */
              if (is_dir)
                dirname = g_strdup (path);
              else
                dirname = g_path_get_dirname (path);
              if (lstat (dirname, &real_dir_st_bufs[i]) != 0)
                {
                  g_set_error (error,
                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid fd passed");
                  return NULL;
                }
            }
          else
            g_ptr_array_index(ids,i) = g_steal_pointer (&id);
        }

      g_ptr_array_index(paths,i) = g_steal_pointer (&path);
    }

  {
    XDP_AUTOLOCK (db); /* Lock once for all ops */

    for (i = 0; i < n_args; i++)
      {
        DocumentAddFullFlags flags;
        DocumentPermissionFlags caller_base_perms = DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS |
                                                    DOCUMENT_PERMISSION_FLAGS_READ;
        DocumentPermissionFlags caller_write_perms = DOCUMENT_PERMISSION_FLAGS_WRITE;
        gboolean reuse_existing, persistent, as_needed_by_app, is_dir;

        flags = documents_flags[i];
        reuse_existing = (flags & DOCUMENT_ADD_FLAGS_REUSE_EXISTING) != 0;
        as_needed_by_app = (flags & DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP) != 0;
        persistent = (flags & DOCUMENT_ADD_FLAGS_PERSISTENT) != 0;
        is_dir = (flags & DOCUMENT_ADD_FLAGS_DIRECTORY) != 0;

        /* If its a unique one its safe for the creator to delete it at will */
        if (!reuse_existing)
          caller_write_perms |= DOCUMENT_PERMISSION_FLAGS_DELETE;

        const char *path = g_ptr_array_index(paths,i);
        g_assert (path != NULL);

        if (as_needed_by_app &&
            app_has_file_access (target_app_id, target_perms, path))
          {
            g_free (g_ptr_array_index(ids,i));
            g_ptr_array_index(ids,i) = g_strdup ("");
            continue;
          }

        if (g_ptr_array_index(ids,i) == NULL)
          {
            char *id = do_create_doc (&real_dir_st_bufs[i], path, reuse_existing, persistent, is_dir);
            g_ptr_array_index(ids,i) = id;

            if (app_id[0] != '\0' && strcmp (app_id, target_app_id) != 0)
              {
                DocumentPermissionFlags caller_perms = caller_base_perms;

                if (writable[i])
                  caller_perms |= caller_write_perms;

                g_autoptr(PermissionDbEntry) entry = permission_db_lookup (db, id);;
                do_set_permissions (entry, id, app_id, caller_perms);
              }

            if (target_app_id[0] != '\0' && target_perms != 0)
              {
                g_autoptr(PermissionDbEntry) entry = permission_db_lookup (db, id);
                do_set_permissions (entry, id, target_app_id, target_perms);
              }
          }
      }
  }

  /* Invalidate with lock dropped to avoid deadlock */
  for (i = 0; i < n_args; i++)
    {
      const char *id = g_ptr_array_index (ids,i);
      g_assert (id != NULL);

      if (*id == 0)
        continue;

      xdp_fuse_invalidate_doc_app (id, NULL);
      if (app_id[0] != '\0')
        xdp_fuse_invalidate_doc_app (id, app_id);
      if (target_app_id[0] != '\0' && target_perms != 0)
        xdp_fuse_invalidate_doc_app (id, target_app_id);
    }

  g_ptr_array_index(ids,n_args) = NULL;

  return g_strdupv ((char**)ids->pdata);
}

static void
portal_add_named_full (GDBusMethodInvocation *invocation,
                       GVariant              *parameters,
                       XdpAppInfo            *app_info)
{
  const char *app_id = xdp_app_info_get_id (app_info);
  GDBusMessage *message;
  GUnixFDList *fd_list;
  int parent_fd_id, parent_fd, fds_len;
  g_autofree char *parent_path = NULL;
  const int *fds = NULL;
  struct stat parent_st_buf;
  gboolean reuse_existing, persistent, as_needed_by_app;
  guint32 flags = 0;
  const char *filename;
  const char *target_app_id;
  g_autofree const char **permissions = NULL;
  g_autofree char *id = NULL;
  g_autofree char *path = NULL;
  DocumentPermissionFlags target_perms;
  GVariantBuilder builder;
  g_autoptr(GVariant) filename_v = NULL;
  GError *error = NULL;

  g_variant_get (parameters, "(h@ayus^a&s)", &parent_fd_id, &filename_v, &flags, &target_app_id, &permissions);
  filename = g_variant_get_bytestring (filename_v);

  /* This is only allowed from the host, or else we could leak existence of files */
  if (!xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  if ((flags & ~DOCUMENT_ADD_FLAGS_FLAGS_ALL) != 0 ||
      /* Don't support directory named documents */
      (flags & DOCUMENT_ADD_FLAGS_DIRECTORY) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid flags");
      return;
    }

  reuse_existing = (flags & DOCUMENT_ADD_FLAGS_REUSE_EXISTING) != 0;
  persistent = (flags & DOCUMENT_ADD_FLAGS_PERSISTENT) != 0;
  as_needed_by_app = (flags & DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP) != 0;

  target_perms = xdp_parse_permissions (permissions, &error);
  if (error)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return;
    }

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  parent_fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (parent_fd_id < fds_len)
        parent_fd = fds[parent_fd_id];
    }

  if (strchr (filename, '/') != NULL || *filename == 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid filename passed");
      return;
    }

  parent_path = xdp_app_info_get_path_for_fd (app_info, parent_fd, S_IFDIR, &parent_st_buf, NULL, &error);
  if (parent_path == NULL || parent_st_buf.st_dev == fuse_dev)
    {
      if (parent_path == NULL)
        g_debug ("Invalid fd passed: %s", error->message);
      else
        g_debug ("Invalid fd passed: \"%s\" not on FUSE device", parent_path);

      /* Don't leak any info about real file path existence, etc */
      g_clear_error (&error);
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  path = g_build_filename (parent_path, filename, NULL);

  g_debug ("portal_add_named_full %s", path);

  {
    DocumentPermissionFlags caller_perms =
      DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS |
      DOCUMENT_PERMISSION_FLAGS_READ |
      DOCUMENT_PERMISSION_FLAGS_WRITE;

    /* If its a unique one its safe for the creator to
       delete it at will */
    if (!reuse_existing)
      caller_perms |= DOCUMENT_PERMISSION_FLAGS_DELETE;

    XDP_AUTOLOCK (db);

    if (as_needed_by_app &&
        app_has_file_access (target_app_id, target_perms, path))
      {
        id = g_strdup ("");
      }
    else
      {
        id = do_create_doc (&parent_st_buf, path, reuse_existing, persistent, FALSE);

        if (app_id[0] != '\0' && strcmp (app_id, target_app_id) != 0)
          {
            g_autoptr(PermissionDbEntry) entry = permission_db_lookup (db, id);;
            do_set_permissions (entry, id, app_id, caller_perms);
          }

        if (target_app_id[0] != '\0' && target_perms != 0)
          {
            g_autoptr(PermissionDbEntry) entry = permission_db_lookup (db, id);
            do_set_permissions (entry, id, target_app_id, target_perms);
          }
      }
  }

  /* Invalidate with lock dropped to avoid deadlock */
  g_assert (id != NULL);

  if (*id != 0)
    {
      xdp_fuse_invalidate_doc_app (id, NULL);
      if (app_id[0] != '\0')
        xdp_fuse_invalidate_doc_app (id, app_id);
      if (target_app_id[0] != '\0' && target_perms != 0)
        xdp_fuse_invalidate_doc_app (id, target_app_id);
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "mountpoint",
                         g_variant_new_bytestring (xdp_fuse_get_mountpoint ()));

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s@a{sv})",
                                                        id,
                                                        g_variant_builder_end (&builder)));
}

static void
portal_add_named (GDBusMethodInvocation *invocation,
                  GVariant              *parameters,
                  XdpAppInfo            *app_info)
{
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autofree char *id = NULL;
  int parent_fd_id, parent_fd, fds_len;
  const int *fds;
  g_autofree char *parent_path = NULL;
  g_autofree char *path = NULL;
  struct stat parent_st_buf;
  const char *filename;
  gboolean reuse_existing, persistent;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) filename_v = NULL;

  g_variant_get (parameters, "(h@aybb)", &parent_fd_id, &filename_v, &reuse_existing, &persistent);
  filename = g_variant_get_bytestring (filename_v);

  /* This is only allowed from the host, or else we could leak existence of files */
  if (!xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  parent_fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (parent_fd_id < fds_len)
        parent_fd = fds[parent_fd_id];
    }

  if (strchr (filename, '/') != NULL || *filename == 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid filename passed");
      return;
    }

  parent_path = xdp_app_info_get_path_for_fd (app_info, parent_fd, S_IFDIR, &parent_st_buf, NULL, &local_error);
  if (parent_path == NULL || parent_st_buf.st_dev == fuse_dev)
    {
      if (parent_path == NULL)
        g_debug ("Invalid fd passed: %s", local_error->message);
      else
        g_debug ("Invalid fd passed: \"%s\" not on FUSE device", parent_path);

      /* Don't leak any info about real file path existence, etc */
      g_clear_error (&local_error);
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  path = g_build_filename (parent_path, filename, NULL);

  XDP_AUTOLOCK (db);

  id = do_create_doc (&parent_st_buf, path, reuse_existing, persistent, FALSE);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id));
}

typedef void (*PortalMethod) (GDBusMethodInvocation *invocation,
                              GVariant              *parameters,
                              XdpAppInfo            *app_info);

static gboolean
handle_method (GCallback              method_callback,
               GDBusMethodInvocation *invocation)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  PortalMethod portal_method = (PortalMethod)method_callback;

  app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, &error);
  if (app_info == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    portal_method (invocation, g_dbus_method_invocation_get_parameters (invocation), app_info);

  return TRUE;
}

static gboolean
handle_get_mount_point (XdpDbusDocuments *object, GDBusMethodInvocation *invocation)
{
  if (fuse_dev == 0)
    {
      /* We mustn't reply to this until the FUSE mount point is open for
       * business. */
      g_queue_push_tail (&get_mount_point_invocations, g_object_ref (invocation));
      return TRUE;
    }

  xdp_dbus_documents_complete_get_mount_point (object, invocation, xdp_fuse_get_mountpoint ());
  return TRUE;
}

static gboolean
portal_lookup (GDBusMethodInvocation *invocation,
               GVariant *parameters,
               XdpAppInfo *app_info)
{
  const char *filename;
  g_autofree char *path = NULL;
  xdp_autofd int fd = -1;
  struct stat st_buf, real_dir_st_buf;
  g_autofree char *id = NULL;
  GError *error = NULL;
  gboolean is_dir;

  if (!xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed in sandbox");
      return TRUE;
    }

  g_variant_get (parameters, "(^&ay)", &filename);

  fd = open (filename, O_PATH | O_CLOEXEC);
  if (fd == -1)
    {
      int errsv = errno;
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "%s", g_strerror (errsv));
      return TRUE;
    }

  if (!validate_fd (fd, app_info, VALIDATE_FD_FILE_TYPE_ANY, &st_buf, &real_dir_st_buf, &path, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return TRUE;
    }

  is_dir = S_ISDIR (st_buf.st_mode);

  if (st_buf.st_dev == fuse_dev)
    {
      /* The passed in fd is on the fuse filesystem itself */
      id = xdp_fuse_lookup_id_for_inode (st_buf.st_ino, is_dir, NULL);
      g_debug ("path on fuse, id %s", id);
    }
  else
    {
      g_autoptr(GVariant) data = NULL;
      g_autoptr(GVariant) data_transient = NULL;
      g_auto(GStrv) ids = NULL;
      guint32 flags = 0;

      if (is_dir)
        flags |= DOCUMENT_ENTRY_FLAG_DIRECTORY;

      data = g_variant_ref_sink (g_variant_new ("(^ayttu)",
                                                path,
                                                (guint64)real_dir_st_buf.st_dev,
                                                (guint64)real_dir_st_buf.st_ino,
                                                flags));
      ids = permission_db_list_ids_by_value (db, data);
      if (ids[0] != NULL)
        id = g_strdup (ids[0]);

      if (id == NULL)
        {
          g_auto(GStrv) transient_ids = NULL;
          data_transient = g_variant_ref_sink (g_variant_new ("(^ayttu)",
                                                              path,
                                                              (guint64)real_dir_st_buf.st_dev,
                                                              (guint64)real_dir_st_buf.st_ino,
                                                              flags|DOCUMENT_ENTRY_FLAG_TRANSIENT));
          transient_ids = permission_db_list_ids_by_value (db, data_transient);
          if (transient_ids[0] != NULL)
            id = g_strdup (transient_ids[0]);
        }
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id ? id : ""));

  return TRUE;
}

static GVariant *
get_app_permissions (PermissionDbEntry *entry)
{
  g_autofree const char **apps = NULL;
  GVariantBuilder builder;
  int i;

  apps = permission_db_entry_list_apps (entry);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));

  for (i = 0; apps[i] != NULL; i++)
    {
      g_autofree const char **permissions = permission_db_entry_list_permissions (entry, apps[i]);
      g_variant_builder_add_value (&builder,
                                   g_variant_new ("{s^as}", apps[i], permissions));
    }

  return g_variant_builder_end (&builder);
}

static GVariant *
get_path (PermissionDbEntry *entry)
{
  g_autoptr (GVariant) data = permission_db_entry_get_data (entry);
  const char *path;

  g_variant_get (data, "(^&ayttu)", &path, NULL, NULL, NULL);
  return g_variant_new_bytestring (path);
}

static gboolean
portal_info (GDBusMethodInvocation *invocation,
             GVariant *parameters,
             XdpAppInfo *app_info)
{
  const char *id = NULL;
  g_autoptr(PermissionDbEntry) entry = NULL;

  if (!xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed in sandbox");
      return TRUE;
    }

  g_variant_get (parameters, "(&s)", &id);

  XDP_AUTOLOCK (db);

  entry = permission_db_lookup (db, id);

  if (!entry)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid ID passed");
      return TRUE;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@ay@a{sas})",
                                                        get_path (entry),
                                                        get_app_permissions (entry)));

  return TRUE;
}

static gboolean
portal_list (GDBusMethodInvocation *invocation,
             GVariant *parameters,
             XdpAppInfo *app_info)
{
  const char *app_id = xdp_app_info_get_id (app_info);
  g_auto(GStrv) ids = NULL;
  GVariantBuilder builder;
  int i;

  if (!xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed in sandbox");
      return TRUE;
    }

  g_variant_get (parameters, "(&s)", &app_id);

  XDP_AUTOLOCK (db);

  if (strcmp (app_id, "") == 0)
    ids = permission_db_list_ids (db);
  else
    ids = permission_db_list_ids_by_app (db, app_id);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{say}"));
  for (i = 0; ids[i]; i++)
    {
      g_autoptr(PermissionDbEntry) entry = NULL;

      entry = permission_db_lookup (db, ids[i]);

      g_variant_builder_add (&builder, "{s@ay}", ids[i], get_path (entry));
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@a{say})",
                                                        g_variant_builder_end (&builder)));

  return TRUE;
}

static void
peer_died_cb (const char *name)
{
  stop_file_transfers_for_sender (name);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;
  GDBusInterfaceSkeleton *file_transfer;

  dbus_api = xdp_dbus_documents_skeleton_new ();

  xdp_dbus_documents_set_version (XDP_DBUS_DOCUMENTS (dbus_api), 4);

  g_signal_connect_swapped (dbus_api, "handle-get-mount-point", G_CALLBACK (handle_get_mount_point), NULL);
  g_signal_connect_swapped (dbus_api, "handle-add", G_CALLBACK (handle_method), portal_add);
  g_signal_connect_swapped (dbus_api, "handle-add-named", G_CALLBACK (handle_method), portal_add_named);
  g_signal_connect_swapped (dbus_api, "handle-add-full", G_CALLBACK (handle_method), portal_add_full);
  g_signal_connect_swapped (dbus_api, "handle-add-named-full", G_CALLBACK (handle_method), portal_add_named_full);
  g_signal_connect_swapped (dbus_api, "handle-grant-permissions", G_CALLBACK (handle_method), portal_grant_permissions);
  g_signal_connect_swapped (dbus_api, "handle-revoke-permissions", G_CALLBACK (handle_method), portal_revoke_permissions);
  g_signal_connect_swapped (dbus_api, "handle-delete", G_CALLBACK (handle_method), portal_delete);
  g_signal_connect_swapped (dbus_api, "handle-lookup", G_CALLBACK (handle_method), portal_lookup);
  g_signal_connect_swapped (dbus_api, "handle-info", G_CALLBACK (handle_method), portal_info);
  g_signal_connect_swapped (dbus_api, "handle-list", G_CALLBACK (handle_method), portal_list);

  file_transfer = file_transfer_create ();
  g_dbus_interface_skeleton_set_flags (file_transfer,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  xdp_connection_track_name_owners (connection, peer_died_cb);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (dbus_api),
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }

  g_debug ("Providing portal %s", g_dbus_interface_skeleton_get_info (G_DBUS_INTERFACE_SKELETON (dbus_api))->name);

  if (!g_dbus_interface_skeleton_export (file_transfer,
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }

  g_debug ("Providing portal %s", g_dbus_interface_skeleton_get_info (G_DBUS_INTERFACE_SKELETON (file_transfer))->name);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  struct stat stbuf;
  gpointer invocation;

  g_debug ("%s acquired", name);

  if (!xdp_fuse_init (&exit_error))
    {
      final_exit_status = 6;
      g_printerr ("fuse init failed: %s", exit_error->message);
      g_main_loop_quit (loop);
      return;
    }

  if (stat (xdp_fuse_get_mountpoint (), &stbuf) != 0)
    {
      g_set_error (&exit_error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "fuse stat failed: %s", g_strerror (errno));
      final_exit_status = 7;
      g_printerr ("fuse stat failed: %s", g_strerror (errno));
      g_main_loop_quit (loop);
      return;
    }

  fuse_dev = stbuf.st_dev;

  xdp_set_documents_mountpoint (xdp_fuse_get_mountpoint ());

  while ((invocation = g_queue_pop_head (&get_mount_point_invocations)) != NULL)
    {
      xdp_dbus_documents_complete_get_mount_point (dbus_api, invocation, xdp_fuse_get_mountpoint ());
      g_object_unref (invocation);
    }
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("%s lost", name);

  if (final_exit_status == 0)
    final_exit_status = 20;

  if (exit_error == NULL)
    g_set_error (&exit_error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "D-Bus name \"%s\" lost", name);

  g_main_loop_quit (loop);
}

gboolean
on_fuse_unmount (void *unused)
{
  if (!g_main_loop_is_running (loop))
    return G_SOURCE_REMOVE;

  g_debug ("fuse fs unmounted externally");

 if (final_exit_status == 0)
   final_exit_status = 21;

  if (exit_error == NULL)
    g_set_error (&exit_error, G_IO_ERROR, G_IO_ERROR_FAILED, "Fuse filesystem unmounted");

  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static void
exit_handler (int sig)
{
  /* We cannot set exit_error here, because malloc() in a signal handler
   * is undefined behaviour. Rely on main() coping gracefully with
   * that. */
  g_main_loop_quit (loop);
}

static void
session_bus_closed (GDBusConnection *connection,
                    gboolean         remote_peer_vanished,
                    GError          *bus_error)
{
  if (exit_error == NULL)
    g_set_error (&exit_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE, "Disconnected from session bus");

  g_main_loop_quit (loop);
}

static int
set_one_signal_handler (int    sig,
                        void (*handler)(int),
                        int    remove)
{
  struct sigaction sa;
  struct sigaction old_sa;

  memset (&sa, 0, sizeof (struct sigaction));
  sa.sa_handler = remove ? SIG_DFL : handler;
  sigemptyset (&(sa.sa_mask));
  sa.sa_flags = 0;

  if (sigaction (sig, NULL, &old_sa) == -1)
    {
      g_warning ("cannot get old signal handler");
      return -1;
    }

  if (old_sa.sa_handler == (remove ? handler : SIG_DFL) &&
      sigaction (sig, &sa, NULL) == -1)
    {
      g_warning ("cannot set signal handler");
      return -1;
    }

  return 0;
}

static gboolean opt_verbose;
static gboolean opt_replace;
static gboolean opt_version;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version and exit", NULL },
  { NULL }
};

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    printf ("XDP: %s\n", message);
  else
    printf ("%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  fprintf (stderr, "error: %s\n", string);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;

  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  GDBusConnection *session_bus;
  g_autoptr(GOptionContext) context = NULL;
  GDBusMethodInvocation *invocation;

  g_log_writer_default_set_use_stderr (TRUE);

  setlocale (LC_ALL, "");

  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_printerr_handler (printerr_handler);

  context = g_option_context_new ("- document portal");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s", error->message);
      return 1;
    }

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  loop = g_main_loop_new (NULL, FALSE);

  path = g_build_filename (g_get_user_data_dir (), "flatpak/db", TABLE_NAME, NULL);
  db = permission_db_new (path, FALSE, &error);
  if (db == NULL)
    {
      g_printerr ("Failed to load db from '%s': %s", path, error->message);
      exit (2);
    }

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      exit (3);
    }

  permission_store = xdg_permission_store_proxy_new_sync (session_bus, G_DBUS_PROXY_FLAGS_NONE,
                                                          "org.freedesktop.impl.portal.PermissionStore",
                                                          "/org/freedesktop/impl/portal/PermissionStore",
                                                          NULL, &error);
  if (permission_store == NULL)
    {
      g_print ("No permission store: %s", error->message);
      exit (4);
    }

  /* We want do do our custom post-mainloop exit */
  g_dbus_connection_set_exit_on_close (session_bus, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed), NULL);

  if (set_one_signal_handler (SIGHUP, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGINT, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGTERM, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGPIPE, SIG_IGN, 0) == -1)
    exit (5);

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.portal.Documents",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  while ((invocation = g_queue_pop_head (&get_mount_point_invocations)) != NULL)
    {
      if (exit_error != NULL)
        g_dbus_method_invocation_return_gerror (invocation, exit_error);
      else
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Terminated");

      g_object_unref (invocation);
    }

  xdp_fuse_exit ();

  g_bus_unown_name (owner_id);

  return final_exit_status;
}
