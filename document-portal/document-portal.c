/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "document-portal.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <libglnx.h>

#include "document-portal-dbus.h"
#include "document-portal-fuse.h"
#include "document-store.h"
#include "file-transfer.h"
#include "permission-db.h"
#include "permission-store-dbus.h"
#include "xdp-app-info-registry.h"
#include "xdp-app-info.h"
#include "xdp-utils.h"

#define TABLE_NAME "documents"

static GMainLoop *loop = NULL;
static PermissionDb *db = NULL;
static XdgPermissionStore *permission_store;
static int final_exit_status = 0;
static GError *exit_error = NULL;
static dev_t fuse_dev = 0;
static GQueue get_mount_point_invocations = G_QUEUE_INIT;
static XdpDbusDocuments *dbus_api;

XdpAppInfoRegistry *app_info_registry;

G_LOCK_DEFINE (db);

/* The documents table is not ours alone. The permission store is a separate
 * process writing the same table, so `flatpak permission-remove`,
 * `flatpak permission-reset` and any other PermissionStore client can change
 * an entry behind our back. We keep a private PermissionDb because the FUSE
 * layer needs a synchronous answer, which means a change we do not observe
 * leaves us authorizing access the user has already revoked.
 *
 * Rather than apply the contents of the Changed signal, treat the entry as
 * stale and re-read it. The signal carries the state the store held when it
 * applied the change, so applying it directly means telling our own echoes
 * apart from external ones, which means pairing signals to writes. Re-reading
 * needs neither: whoever made the change, the answer is whatever the store
 * holds now.
 *
 * That only holds while the store is not behind us. Our writes to it are
 * asynchronous, so between applying a change locally and the store
 * acknowledging it, a re-read would undo what we just applied. A Changed
 * arriving in that window is therefore recorded, not acted on, and the read
 * happens once our writes are acknowledged.
 *
 * https://github.com/flatpak/xdg-desktop-portal/issues/197
 */
typedef struct
{
  guint    writes;   /* our writes to the store not yet acknowledged */
  gboolean reading;  /* a Lookup is in flight */
  gboolean queued;   /* a Changed arrived while we could not read */
} EntrySync;

/* id -> EntrySync. Guarded by the db lock. */
static GHashTable *entry_sync;

static void start_refresh (const char *id, EntrySync *sync);

static EntrySync *
entry_sync_get (const char *id)
{
  EntrySync *sync = g_hash_table_lookup (entry_sync, id);

  if (sync == NULL)
    {
      sync = g_new0 (EntrySync, 1);
      g_hash_table_insert (entry_sync, g_strdup (id), sync);
    }

  return sync;
}

static void
entry_sync_release (const char *id,
                    EntrySync  *sync)
{
  if (sync->writes == 0 && !sync->reading && !sync->queued)
    g_hash_table_remove (entry_sync, id);
}

/* Re-read the entry, unless the store is still catching up with us or a read
 * is already under way. Call with the db lock held. */
static void
refresh_entry (const char *id,
               EntrySync  *sync)
{
  if (sync->writes > 0 || sync->reading)
    {
      sync->queued = TRUE;
      return;
    }

  sync->queued = FALSE;
  start_refresh (id, sync);
}

/* One of our writes to the store has been acknowledged, or failed, in which
 * case it emits no Changed and there is nothing further to wait for. */
static void
store_write_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autofree char *id = user_data;

  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;
  EntrySync *sync;

  ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (ret == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Failed to update the permission store for %s: %s", id, error->message);
    }

  XDP_AUTOLOCK (db);

  sync = g_hash_table_lookup (entry_sync, id);
  if (sync == NULL)
    return;

  if (sync->writes > 0)
    sync->writes--;

  if (sync->writes == 0 && sync->queued && !sync->reading)
    refresh_entry (id, sync);
  else
    entry_sync_release (id, sync);
}

/* Call with the db lock held. */
static void
note_store_write (const char *id)
{
  entry_sync_get (id)->writes++;
}


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
      note_store_write (doc_id);
      xdg_permission_store_call_set_permission (permission_store,
                                                TABLE_NAME,
                                                FALSE,
                                                doc_id,
                                                app_id,
                                                perms_s,
                                                NULL,
                                                store_write_cb, g_strdup (doc_id));
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
  g_autoptr(GError) error = NULL;

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
        g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
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
  g_autoptr(GError) error = NULL;

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
        g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
        return;
      }

    /* Must have grant-permissions, or be itself */
    if (!document_entry_has_permissions (entry, app_info,
                                    DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS) ||
        g_strcmp0 (app_id, target_app_id) == 0)
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
      {
        note_store_write (id);
        xdg_permission_store_call_delete (permission_store, TABLE_NAME,
                                          id, NULL, store_write_cb, g_strdup (id));
      }
  }

  /* All i/o is done now, so drop the lock so we can invalidate the fuse caches */
  old_apps = permission_db_entry_list_apps (entry);
  for (i = 0; old_apps[i] != NULL; i++)
    xdp_fuse_invalidate_doc_app (id, old_apps[i]);
  xdp_fuse_invalidate_doc_app (id, NULL);

  /* Now fuse view is up-to-date, so we can return the call */
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

GBytes *
xdp_file_handle_for_fd (int fd)
{
  g_autofree struct file_handle *handle = NULL;
  g_autofd int fd_owned = -1;

  if (!(fcntl (fd, F_GETFL) & O_PATH))
    fd = fd_owned = glnx_fd_reopen (fd, O_PATH, NULL);

  if (fd < 0 ||
      !glnx_name_to_handle_at (fd, "",
                               AT_EMPTY_PATH | AT_HANDLE_FID,
                               &handle,
                               NULL,
                               NULL))
    return NULL;

  return g_bytes_new (handle->f_handle, handle->handle_bytes);
}

typedef struct _FindIdData {
  const char *path;
  dev_t       st_dev;
  ino_t       st_ino;
  GBytes     *handle;
  uint32_t    flags;
  gboolean    ignore_transient;
} FindIdData;

static gboolean
find_id_matches (PermissionDbEntry *entry,
                 gpointer           user_data)
{
  FindIdData *match = user_data;

  if (g_strcmp0 (document_entry_get_path (entry), match->path) != 0)
    return FALSE;

  {
    uint32_t flags = document_entry_get_flags (entry);

    if (match->ignore_transient)
      flags &= ~DOCUMENT_ENTRY_FLAG_TRANSIENT;

    if (match->flags != flags)
      return FALSE;
  }

  if (match->handle)
    {
      g_autoptr(GBytes) handle = NULL;

      handle = document_entry_dup_handle (entry);
      if (!handle || !g_bytes_equal (handle, match->handle))
        return FALSE;
    }
  else
    {
      if (match->st_dev != document_entry_get_device (entry) ||
          match->st_ino != document_entry_get_inode (entry))
        return FALSE;
    }

  return TRUE;
}

static char *
find_id (const char *path,
         dev_t       st_dev,
         ino_t       st_ino,
         GBytes     *handle,
         uint32_t    flags,
         gboolean    ignore_transient)
{
  FindIdData find_data;

  find_data = (FindIdData) {
    .path = path,
    .st_dev = st_dev,
    .st_ino = st_ino,
    .handle = handle,
    .flags = flags,
    .ignore_transient = ignore_transient,
  };

  {
    g_auto(GStrv) ids = NULL;

    ids = permission_db_filter_ids (db, find_id_matches, &find_data);
    if (ids[0] != NULL)
      return g_strdup (ids[0]);
  }

  /* We didn't have a handle, so we already checked by dev+ino */
  if (find_data.handle == NULL)
    return NULL;

  /* We didn't find a match via handle, so fall back to checking by dev+ino */
  {
    g_auto(GStrv) ids = NULL;

    find_data.handle = NULL;

    ids = permission_db_filter_ids (db, find_id_matches, &find_data);
    if (ids[0] != NULL)
      return g_strdup (ids[0]);
  }

  return NULL;
}

static char *
do_create_doc (struct stat *parent_st_buf,
               GBytes      *handle,
               const char  *path,
               gboolean     reuse_existing,
               gboolean     persistent,
               gboolean     directory)
{
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autofree char *id = NULL;
  guint32 flags = 0;

  g_debug ("Creating document at path '%s', reuse_existing: %d, persistent: %d, directory: %d", path, reuse_existing, persistent, directory);

  if (!reuse_existing)
    flags |= DOCUMENT_ENTRY_FLAG_UNIQUE;
  if (!persistent)
    flags |= DOCUMENT_ENTRY_FLAG_TRANSIENT;
  if (directory)
    flags |= DOCUMENT_ENTRY_FLAG_DIRECTORY;

  entry = document_entry_new (path, flags, parent_st_buf->st_dev, parent_st_buf->st_ino, handle);

  if (reuse_existing)
    {
      id = find_id (path,
                    parent_st_buf->st_dev,
                    parent_st_buf->st_ino,
                    handle,
                    flags,
                    FALSE /* ignore_transient */);
    }

  if (id)
    {
      g_debug ("reuse_doc %s", id);
    }
  else
    {
      while (id == NULL)
        {
          g_autoptr(PermissionDbEntry) existing = NULL;

          id = xdp_generate_token ();
          existing = permission_db_lookup (db, id);
          if (existing)
            g_clear_pointer (&id, g_free);
        }

      g_debug ("create_doc %s", id);
    }

  permission_db_set_entry (db, id, entry);

  if (persistent)
    {
      g_autoptr(GVariant) data = permission_db_entry_get_data (entry);

      note_store_write (id);
      xdg_permission_store_call_set (permission_store,
                                     TABLE_NAME,
                                     TRUE,
                                     id,
                                     g_variant_new_array (G_VARIANT_TYPE ("{sas}"), NULL, 0),
                                     g_variant_new_variant (data),
                                     NULL, store_write_cb, g_strdup (id));
    }

  return g_steal_pointer (&id);
}

gboolean
validate_fd (int fd,
             XdpAppInfo *app_info,
             ValidateFdType ensure_type,
             struct stat *st_buf,
             struct stat *real_dir_st_buf,
             GBytes **real_dir_handle_out,
             char **path_out,
             gboolean *writable_out,
             GError **error)
{
  g_autofree char *path = NULL;
  g_autofree char *dirname = NULL;
  g_autofree char *name = NULL;
  g_autofd int dir_fd = -1;
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

  if (real_dir_handle_out)
    *real_dir_handle_out = xdp_file_handle_for_fd (dir_fd);

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
  g_autoptr(GError) error = NULL;
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
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
      return;
    }

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", ids[0]));
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

          if (g_strcmp0 (fs, "!host") == 0)
            *allow_host_out = 0;
          if (g_strcmp0 (fs, "host:ro") == 0)
            *allow_host_out = 1;
          if (g_strcmp0 (fs, "host") == 0)
            *allow_host_out = 2;

          if (g_strcmp0 (fs, "!home") == 0)
            *allow_home_out = 0;
          if (g_strcmp0 (fs, "home:ro") == 0)
            *allow_home_out = 1;
          if (g_strcmp0 (fs, "home") == 0)
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

  if (target_app_id == NULL || !xdp_is_valid_app_id (target_app_id))
    return FALSE;

  if (g_str_has_prefix (target_app_id, "snap."))
    {
      res = xdp_spawn (&error, "snap", "routine", "file-access",
                        target_app_id + strlen ("snap."), path, NULL);
    }
  else
    {
      /* First we try flatpak info --file-access=PATH APPID, which is supported on new versions */
      arg = g_strdup_printf ("--file-access=%s", path);
      res = xdp_spawn (&error, "flatpak", "info", arg, target_app_id, NULL);
    }

  if (res)
    {
      g_strchomp (res);

      if (g_strcmp0 (res, "read-write") == 0)
        return TRUE;

      if (g_strcmp0 (res, "read-only") == 0 &&
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
  g_autoptr(GError) error = NULL;
  GVariantBuilder builder;
  int fds_len;
  int i;
  const int *fds;

  g_variant_get (parameters, "(@ahu&s^a&s)",
                 &array, &flags, &target_app_id, &permissions);

  if (target_app_id[0] != '\0' &&
      !xdp_is_valid_app_id (target_app_id))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "'%s' is not a valid app name", target_app_id);
      return;
    }

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
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
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
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
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
                   dev_t                    *parent_dev,
                   ino_t                    *parent_ino,
                   DocumentAddFullFlags     *documents_flags,
                   int                       n_args,
                   XdpAppInfo               *app_info,
                   const char               *target_app_id,
                   DocumentPermissionFlags   target_perms,
                   GError                  **error)
{
  /* gobject-linter-ignore-next-line: use_auto_cleanup */
  const char *app_id = xdp_app_info_get_id (app_info);
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) handles = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  g_autofree struct stat *real_dir_st_bufs = NULL;
  struct stat st_buf;
  g_autofree gboolean *writable = NULL;
  int i;

  g_ptr_array_set_size (paths, n_args + 1);
  g_ptr_array_set_size (ids, n_args + 1);
  g_ptr_array_set_size (handles, n_args + 1);
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

      if (!validate_fd (fd[i], app_info,
                        is_dir ? VALIDATE_FD_FILE_TYPE_DIR : VALIDATE_FD_FILE_TYPE_REGULAR,
                        &st_buf, &real_dir_st_bufs[i],
                        (GBytes **)&g_ptr_array_index (handles, i),
                        &path, &writable[i], error))
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
              g_autofd int dir_fd = -1;
              g_autoptr(GBytes) old_handle = NULL;

              g_free (path);
              path = g_steal_pointer (&real_path);
              /* Need to update real_dir_st_bufs */
              if (is_dir)
                dirname = g_strdup (path);
              else
                dirname = g_path_get_dirname (path);

              dir_fd = open (dirname, O_CLOEXEC | O_PATH);
              if (dir_fd < 0 || fstat (dir_fd, &real_dir_st_bufs[i]) != 0)
                {
                  g_set_error (error,
                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid fd passed");
                  return NULL;
                }

              old_handle = g_steal_pointer (&g_ptr_array_index (handles, i));
              g_ptr_array_index (handles, i) = xdp_file_handle_for_fd (dir_fd);
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
            g_set_str ((char **) &g_ptr_array_index (ids, i), "");
            continue;
          }

        if (g_ptr_array_index(ids,i) == NULL)
          {
            char *id = do_create_doc (&real_dir_st_bufs[i], g_ptr_array_index (handles, i), path, reuse_existing, persistent, is_dir);
            g_ptr_array_index(ids,i) = id;

            if (app_id[0] != '\0' && g_strcmp0 (app_id, target_app_id) != 0)
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
      /* gobject-linter-ignore-next-line: use_auto_cleanup */
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
  g_autoptr(GBytes) handle = NULL;
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
  g_autoptr(GError) error = NULL;

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

  if (target_app_id[0] != '\0' &&
      !xdp_is_valid_app_id (target_app_id))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "'%s' is not a valid app name", target_app_id);
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
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
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

  if (!xdp_is_valid_filename (filename))
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
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  handle = xdp_file_handle_for_fd (parent_fd);
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
        id = do_create_doc (&parent_st_buf, handle, path, reuse_existing, persistent, FALSE);

        if (app_id[0] != '\0' && g_strcmp0 (app_id, target_app_id) != 0)
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
  g_autoptr(GBytes) handle = NULL;
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

  if (!xdp_is_valid_filename (filename))
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

  handle = xdp_file_handle_for_fd (parent_fd);
  path = g_build_filename (parent_path, filename, NULL);

  XDP_AUTOLOCK (db);

  id = do_create_doc (&parent_st_buf, handle, path, reuse_existing, persistent, FALSE);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id));
}

typedef void (*PortalMethod) (GDBusMethodInvocation *invocation,
                              GVariant              *parameters,
                              XdpAppInfo            *app_info);


static DexFuture *
handle_method_with_app_info (DexFuture *completed,
                             gpointer   user_data)
{
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (user_data);
  PortalMethod portal_method = g_object_steal_data (G_OBJECT (invocation),
                                                    "-xdp-portal-method");
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  app_info = dex_await_object (dex_ref (completed), &error);
  if (app_info == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    portal_method (invocation, g_dbus_method_invocation_get_parameters (invocation), app_info);

  return dex_future_new_true ();
}

static gboolean
handle_method (GCallback              method_callback,
               GDBusMethodInvocation *invocation)
{
  g_autoptr(DexFuture) future = NULL;

  g_object_set_data (G_OBJECT (invocation), "-xdp-portal-method", method_callback);

  future = xdp_app_info_registry_ensure_future (app_info_registry, invocation);
  future = dex_future_finally (future,
                               handle_method_with_app_info,
                               invocation,
                               NULL);
  dex_future_disown (g_steal_pointer (&future));

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
  g_autofd int fd = -1;
  struct stat st_buf, real_dir_st_buf;
  g_autoptr(GBytes) handle = NULL;
  g_autofree char *id = NULL;
  g_autoptr(GError) error = NULL;
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

  if (!validate_fd (fd, app_info, VALIDATE_FD_FILE_TYPE_ANY, &st_buf, &real_dir_st_buf, &handle, &path, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&error));
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
      id = find_id (path,
                    real_dir_st_buf.st_dev,
                    real_dir_st_buf.st_ino,
                    handle,
                    is_dir ? DOCUMENT_ENTRY_FLAG_DIRECTORY : 0,
                    TRUE /* ignore_transient */);
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
  const char *path = document_entry_get_path (entry);

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

  if (g_strcmp0 (app_id, "") == 0)
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

const char *
get_host_path_internal (GDBusMethodInvocation *invocation,
                        XdpAppInfo            *app_info,
                        const char            *id,
                        GError                **error)
{
  g_autoptr(PermissionDbEntry) entry = NULL;

  XDP_AUTOLOCK (db);

  entry = permission_db_lookup (db, id);

  if (!entry)
    {
      if (error != NULL && *error == NULL)
        {
          g_set_error (error,
                      XDG_DESKTOP_PORTAL_ERROR,
                      XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                      "Invalid ID passed (%s)", id);
        }

      return NULL;
    }

  if (!xdp_app_info_is_host (app_info))
    {
      g_autofree const char **apps = NULL;
      const char *app_id = NULL;
      gboolean app_found = FALSE;

      app_id = xdp_app_info_get_id (app_info);

      apps = permission_db_entry_list_apps (entry);
      for (size_t i = 0; apps[i] != NULL; i++)
        {
          if (g_strcmp0 (app_id, apps[i]) == 0)
            {
              app_found = TRUE;
              break;
            }
        }

      if (!app_found)
        {
          if (error != NULL && *error == NULL)
            {
              g_set_error (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                           "Not enough permissions");
            }

          return NULL;
        }
    }

  return document_entry_get_path (entry);
}

static gboolean
portal_get_host_paths (GDBusMethodInvocation *invocation,
                       GVariant              *parameters,
                       XdpAppInfo            *app_info)
{
  g_autofree const char **id_list = NULL;
  GVariantBuilder builder;

  g_variant_get (parameters, "(^a&s)", &id_list);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("(a{say})"));
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{say}"));

  for (size_t i = 0; id_list[i] != NULL; i++)
    {
      g_autoptr(GError) error = NULL;
      const char *path = NULL;

      path = get_host_path_internal (invocation, app_info, id_list[i], &error);
      if (path == NULL)
        {
          g_warning ("Failed to get host path for %s: %s", id_list[i], error->message);
          continue;
        }

      g_variant_builder_add (&builder, "{s@ay}", id_list[i], g_variant_new_bytestring (path));
    }

  g_variant_builder_close (&builder);

  g_dbus_method_invocation_return_value (invocation, g_variant_builder_end (&builder));

  return TRUE;
}

static void
on_peer_disconnect (const char *name,
                    gpointer    user_data)
{
  stop_file_transfers_for_sender (name);

  dex_future_disown (xdp_app_info_registry_delete_future (app_info_registry,
                                                          name));
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *file_transfer;

  dbus_api = xdp_dbus_documents_skeleton_new ();

  app_info_registry = xdp_app_info_registry_new ();

  xdp_dbus_documents_set_version (XDP_DBUS_DOCUMENTS (dbus_api), 5);

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
  g_signal_connect_swapped (dbus_api, "handle-get-host-paths", G_CALLBACK (handle_method), portal_get_host_paths);

  file_transfer = file_transfer_create ();
  g_dbus_interface_skeleton_set_flags (file_transfer,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  xdp_connection_track_peer_disconnect (connection, on_peer_disconnect, NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (dbus_api),
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
    }

  g_debug ("Providing portal %s", g_dbus_interface_skeleton_get_info (G_DBUS_INTERFACE_SKELETON (dbus_api))->name);

  if (!g_dbus_interface_skeleton_export (file_transfer,
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
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

static gboolean
exit_handler (gpointer user_data)
{
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
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
    fprintf (stderr, "XDP: %s\n", message);
  else
    fprintf (stderr, "%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  int is_tty = isatty (1);
  const char *prefix = "";
  const char *suffix = "";
  if (is_tty)
    {
      prefix = "\x1b[31m\x1b[1m"; /* red, bold */
      suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
    }
  fprintf (stderr, "%serror: %s%s\n", prefix, suffix, string);
}

static gboolean
permissions_equal (const char **a,
                   const char **b)
{
  gsize i;

  if (a == NULL || b == NULL)
    return a == b;

  if (g_strv_length ((char **) a) != g_strv_length ((char **) b))
    return FALSE;

  for (i = 0; a[i] != NULL; i++)
    {
      if (!g_strv_contains ((const char * const *) b, a[i]))
        return FALSE;
    }

  return TRUE;
}

/* Replace our copy of an entry's permissions with the ones the store holds.
 * Only permissions are taken: the document metadata is ours and the store
 * keeps a mirror of it. Returns the apps whose permissions changed, whose FUSE
 * nodes must be invalidated once the db lock is dropped.
 * Call with the db lock held. */
static GPtrArray *
apply_store_permissions (const char *id,
                         GVariant   *permissions)
{
  g_autoptr(PermissionDbEntry) old_entry = NULL;
  g_autoptr(PermissionDbEntry) new_entry = NULL;
  g_autofree const char **old_apps = NULL;
  GPtrArray *changed_apps;
  GVariantIter iter;
  const char *app_id;
  GVariant *app_permissions;
  gsize i;

  changed_apps = g_ptr_array_new_with_free_func (g_free);

  old_entry = permission_db_lookup (db, id);

  /* An id we do not know about. We cannot usefully add it: the document
   * metadata is ours and the store only mirrors it. Losing a grant is what
   * matters here, and that cannot happen for an entry we do not have. */
  if (old_entry == NULL)
    return changed_apps;

  old_apps = permission_db_entry_list_apps (old_entry);
  new_entry = permission_db_entry_ref (old_entry);

  /* Apps the store no longer lists have lost their grant entirely */
  for (i = 0; old_apps[i] != NULL; i++)
    {
      g_autoptr(GVariant) still_listed = NULL;

      still_listed = g_variant_lookup_value (permissions, old_apps[i],
                                             G_VARIANT_TYPE_STRING_ARRAY);
      if (still_listed == NULL)
        {
          g_autoptr(PermissionDbEntry) prev = g_steal_pointer (&new_entry);

          new_entry = permission_db_entry_remove_app_permissions (prev, old_apps[i]);
          g_ptr_array_add (changed_apps, g_strdup (old_apps[i]));
        }
    }

  g_variant_iter_init (&iter, permissions);
  while (g_variant_iter_loop (&iter, "{&s@as}", &app_id, &app_permissions))
    {
      g_autofree const char **new_perms = NULL;
      g_autofree const char **old_perms = NULL;
      g_autoptr(PermissionDbEntry) prev = NULL;

      new_perms = g_variant_get_strv (app_permissions, NULL);
      old_perms = permission_db_entry_list_permissions (old_entry, app_id);

      if (permissions_equal (old_perms, new_perms))
        continue;

      prev = g_steal_pointer (&new_entry);
      new_entry = permission_db_entry_set_app_permissions (prev, app_id, new_perms);
      g_ptr_array_add (changed_apps, g_strdup (app_id));
    }

  if (changed_apps->len > 0)
    permission_db_set_entry (db, id, new_entry);

  return changed_apps;
}

/* Drop an entry the store no longer has. Call with the db lock held. */
static GPtrArray *
forget_entry (const char *id,
              gboolean   *entry_removed)
{
  g_autoptr(PermissionDbEntry) old_entry = NULL;
  g_autofree const char **old_apps = NULL;
  GPtrArray *changed_apps;
  gsize i;

  changed_apps = g_ptr_array_new_with_free_func (g_free);
  *entry_removed = FALSE;

  old_entry = permission_db_lookup (db, id);
  if (old_entry == NULL)
    return changed_apps;

  old_apps = permission_db_entry_list_apps (old_entry);
  for (i = 0; old_apps[i] != NULL; i++)
    g_ptr_array_add (changed_apps, g_strdup (old_apps[i]));

  permission_db_set_entry (db, id, NULL);
  *entry_removed = TRUE;

  return changed_apps;
}

static void
refresh_lookup_cb (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autofree char *id = user_data;

  g_autoptr(GVariant) permissions = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) changed_apps = NULL;
  gboolean entry_removed = FALSE;
  gboolean gone = FALSE;
  EntrySync *sync;
  gsize i;

  if (!xdg_permission_store_call_lookup_finish (XDG_PERMISSION_STORE (source_object),
                                                &permissions, &data, res, &error))
    {
      g_autofree char *remote_error = g_dbus_error_get_remote_error (error);

      /* Not g_error_matches(): the D-Bus name mapping for
       * XDG_DESKTOP_PORTAL_ERROR is registered lazily, on first use of the
       * quark, so a reply decoded before anything in this process has used
       * that domain lands in G_IO_ERROR instead and the match fails. The wire
       * name is there either way. */
      gone = g_strcmp0 (remote_error, "org.freedesktop.portal.Error.NotFound") == 0;

      if (!gone)
        {
          /* Leave our copy alone. For a revocation we have already applied it
           * is the more restrictive of the two, and a transient bus error is
           * not evidence that a grant is gone. */
          g_dbus_error_strip_remote_error (error);
          g_warning ("Failed to re-read permissions for %s: %s", id, error->message);
        }
    }

  {
    XDP_AUTOLOCK (db);

    if (gone)
      changed_apps = forget_entry (id, &entry_removed);
    else if (permissions != NULL)
      changed_apps = apply_store_permissions (id, permissions);

    sync = g_hash_table_lookup (entry_sync, id);
    if (sync != NULL)
      {
        sync->reading = FALSE;

        if (sync->queued)
          refresh_entry (id, sync);
        else
          entry_sync_release (id, sync);
      }
  }

  /* All i/o is done now, so drop the lock so we can invalidate the fuse caches */
  for (i = 0; changed_apps != NULL && i < changed_apps->len; i++)
    xdp_fuse_invalidate_doc_app (id, g_ptr_array_index (changed_apps, i));

  if (entry_removed)
    xdp_fuse_invalidate_doc_app (id, NULL);
}

/* Call with the db lock held. */
static void
start_refresh (const char *id,
               EntrySync  *sync)
{
  sync->reading = TRUE;
  xdg_permission_store_call_lookup (permission_store, TABLE_NAME, id, NULL,
                                    refresh_lookup_cb, g_strdup (id));
}

static void
on_permission_store_changed (XdgPermissionStore *store,
                             const char         *table_name,
                             const char         *id,
                             gboolean            deleted,
                             GVariant           *data,
                             GVariant           *permissions,
                             gpointer            user_data)
{
  if (g_strcmp0 (table_name, TABLE_NAME) != 0)
    return;

  XDP_AUTOLOCK (db);

  refresh_entry (id, entry_sync_get (id));
}

/* The store restarting is a gap in the Changed stream: whatever happened while
 * it was away was never announced, and it comes back from whatever reached
 * disk, which can be behind what it had already told us. Re-read everything we
 * hold rather than guess which entries moved. */
static void
on_permission_store_name_owner_changed (GObject    *object,
                                        GParamSpec *pspec,
                                        gpointer    user_data)
{
  g_autofree char *owner = NULL;
  g_auto(GStrv) ids = NULL;
  gsize i;

  owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (object));
  if (owner == NULL)
    return;

  XDP_AUTOLOCK (db);

  ids = permission_db_list_ids (db);
  for (i = 0; ids != NULL && ids[i] != NULL; i++)
    refresh_entry (ids[i], entry_sync_get (ids[i]));
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;

  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(PermissionDb) owned_db = NULL;
  GDBusMethodInvocation *invocation;

  if (g_getenv ("XDG_DOCUMENT_PORTAL_WAIT_FOR_DEBUGGER") != NULL)
    {
      g_printerr ("document portal (PID %d) is waiting for a debugger. "
                  "Use `gdb -p %d` to connect. \n",
                  getpid (), getpid ());

      if (raise (SIGSTOP) == -1)
        {
          g_printerr ("Failed waiting for debugger\n");
          exit (1);
        }

      raise (SIGCONT);
    }

  g_log_writer_default_set_use_stderr (TRUE);

  setlocale (LC_ALL, "");

  dex_init ();

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
  db = owned_db = permission_db_new (path, FALSE, &error);
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

  entry_sync = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* We are not the only writer of the documents table, so re-read entries the
   * permission store tells us have changed rather than trusting our copy */
  g_signal_connect (permission_store, "changed",
                    G_CALLBACK (on_permission_store_changed), NULL);
  g_signal_connect (permission_store, "notify::g-name-owner",
                    G_CALLBACK (on_permission_store_name_owner_changed), NULL);

  /* We want do do our custom post-mainloop exit */
  g_dbus_connection_set_exit_on_close (session_bus, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed), NULL);

  if (g_unix_signal_add (SIGHUP, exit_handler, NULL) == 0 ||
      g_unix_signal_add (SIGINT, exit_handler, NULL) == 0 ||
      g_unix_signal_add (SIGTERM, exit_handler, NULL) == 0 ||
      signal (SIGPIPE, SIG_IGN) == SIG_ERR)
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
