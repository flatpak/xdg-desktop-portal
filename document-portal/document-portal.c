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
#include "document-portal-dbus.h"
#include "document-store.h"
#include "src/xdp-utils.h"
#include "permission-db.h"
#include "permission-store-dbus.h"
#include "document-portal-fuse.h"

#include <sys/eventfd.h>

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
static int daemon_event_fd = -1;
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
  const char *app_id = xdp_app_info_get_id (app_info);
  const char *target_app_id;
  const char *id;
  g_autofree const char **permissions = NULL;
  DocumentPermissionFlags perms;

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

    if (!xdp_is_valid_flatpak_name (target_app_id))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "'%s' is not a valid app name", target_app_id);
        return;
      }

    perms = xdp_parse_permissions (permissions);

    /* Must have grant-permissions and all the newly granted permissions */
    if (!document_entry_has_permissions (entry, app_id,
                                    DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS | perms))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    do_set_permissions (entry, id, target_app_id,
                        perms | document_entry_get_permissions (entry, target_app_id));
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

    if (!xdp_is_valid_flatpak_name (target_app_id))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "'%s' is not a valid app name", target_app_id);
        return;
      }

    perms = xdp_parse_permissions (permissions);

    /* Must have grant-permissions, or be itself */
    if (!document_entry_has_permissions (entry, app_id,
                                    DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS) ||
        strcmp (app_id, target_app_id) == 0)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    do_set_permissions (entry, id, target_app_id,
                        ~perms & document_entry_get_permissions (entry, target_app_id));
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
  const char *app_id = xdp_app_info_get_id (app_info);
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autofree const char **old_apps = NULL;
  int i;

  g_variant_get (parameters, "(s)", &id);

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

    if (!document_entry_has_permissions (entry, app_id, DOCUMENT_PERMISSION_FLAGS_DELETE))
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
do_create_doc (struct stat *parent_st_buf, const char *path, gboolean reuse_existing, gboolean persistent)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_auto(GStrv) ids = NULL;
  char *id = NULL;
  guint32 flags = 0;

  g_debug ("Creating document at path '%s', resuse_existing: %d, persistent: %d", path, reuse_existing, persistent);

  if (!reuse_existing)
    flags |= DOCUMENT_ENTRY_FLAG_UNIQUE;
  if (!persistent)
    flags |= DOCUMENT_ENTRY_FLAG_TRANSIENT;
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

static int
get_fd_access (int fd)
{
  g_autofree char *p = g_strdup_printf ("/proc/self/fd/%d", fd);
  int mode;
  int res;

  mode = 0;

  res = access (p, R_OK);
  if (res == 0)
    mode |= R_OK;
  else if (errno != EACCES && errno != EROFS)
    return -1;

  res = access (p, W_OK);
  if (res == 0)
    mode |= W_OK;
  else if (errno != EACCES && errno != EROFS)
    return -1;

  res = access (p, X_OK);
  if (res == 0)
    mode |= X_OK;
  else if (errno != EACCES && errno != EROFS)
    return -1;

  return mode;
}

static gboolean
verify_proc_self_fd (int fd,
                     char *path_buffer)
{
  g_autofree char *proc_path = NULL;
  ssize_t symlink_size;

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  symlink_size = readlink (proc_path, path_buffer, PATH_MAX);
  if (symlink_size < 0)
    return FALSE;

  path_buffer[symlink_size] = 0;

  /* All normal paths start with /, but some weird things
     don't, such as socket:[27345] or anon_inode:[eventfd].
     We don't support any of these */
  if (path_buffer[0] != '/')
    return FALSE;

  /* File descriptors to actually deleted files have " (deleted)"
     appended to them. This also happens to some fake fd types
     like shmem which are "/<name> (deleted)". All such
     files are considered invalid. Unfortunatelly this also
     matches files with filenames that actually end in " (deleted)",
     but there is not much to do about this. */
  if (g_str_has_suffix (path_buffer, " (deleted)"))
    return FALSE;

  return TRUE;
}

static gboolean
validate_fd_common (int fd,
                    XdpAppInfo *app_info,
                    struct stat *st_buf,
                    mode_t st_mode,
                    char **path_out,
                    int *access_mode_out,
                    GError **error)
{
  int fd_flags;
  int access_mode;
  int required_mode;
  char path_buffer[PATH_MAX + 1];

  if (fd == -1 ||
      /* Must be able to get fd flags */
      (fd_flags = fcntl (fd, F_GETFL)) == -1 ||
      /* Must be O_PATH */
      ((fd_flags & O_PATH) != O_PATH) ||
      /* Must not be O_NOFOLLOW (because we want the target file) */
      ((fd_flags & O_NOFOLLOW) == O_NOFOLLOW) ||
      /* Must be able to fstat */
      fstat (fd, st_buf) < 0 ||
      /* Must be a regular file or directory (depending on use) */
      (st_buf->st_mode & S_IFMT) != st_mode ||
      /* Must be able to read path from /proc/self/fd */
      /* This is an absolute and (at least at open time) symlink-expanded path */
      (verify_proc_self_fd (fd, path_buffer)) < 0)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid fd passed");
      return FALSE;
    }

  /* we need at least read access, but also execute for dirs */
  required_mode = R_OK;
  if (st_mode == S_IFDIR)
    required_mode |= X_OK;

  access_mode = get_fd_access (fd);
  if (access_mode < 0 || (access_mode & required_mode) != required_mode)
    {
      /* Don't leak any info about real file path existence, etc */
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid fd passed");
      return FALSE;
    }

  if (path_out)
    *path_out = xdp_app_info_remap_path (app_info, path_buffer);

  if (access_mode_out)
    *access_mode_out = access_mode;

  return TRUE;
}

static gboolean
validate_parent_dir (const char *path,
                     struct stat *st_buf,
                     struct stat *real_parent_st_buf,
                     GError **error)
{
  g_autofree char *dirname = NULL;
  g_autofree char *name = NULL;
  xdp_autofd int dir_fd = -1;
  struct stat real_st_buf;

  /* We open the parent directory and do the stat in that, so that we have
   * trustworthy parent dev/ino for later verification. Otherwise the caller
   * could later replace a parent with a symlink and make us read some other file
   */
  dirname = g_path_get_dirname (path);
  name = g_path_get_basename (path);
  dir_fd = open (dirname, O_CLOEXEC | O_PATH);

  if (dir_fd < 0 ||
      fstat (dir_fd, real_parent_st_buf) < 0 ||
      fstatat (dir_fd, name, &real_st_buf, AT_SYMLINK_NOFOLLOW) < 0 ||
      st_buf->st_dev != real_st_buf.st_dev ||
      st_buf->st_ino != real_st_buf.st_ino)
    {
      /* Don't leak any info about real file path existence, etc */
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid fd passed");
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_fd (int fd,
             XdpAppInfo *app_info,
             struct stat *st_buf,
             struct stat *real_parent_st_buf,
             char **path_out,
             int *access_mode_out,
             GError **error)
{
  g_autofree char *remapped_path = NULL;

  if (!validate_fd_common (fd, app_info, st_buf, S_IFREG, &remapped_path, access_mode_out, error))
    return FALSE;

  if (!validate_parent_dir (remapped_path, st_buf, real_parent_st_buf, error))
    return FALSE;

  if (path_out)
    *path_out = g_steal_pointer (&remapped_path);

  return TRUE;
}

static char *
verify_existing_document (struct stat *st_buf, gboolean reuse_existing)
{
  g_autoptr(PermissionDbEntry) old_entry = NULL;
  g_autofree char *id = NULL;

  g_assert (st_buf->st_dev == fuse_dev);

  /* The passed in fd is on the fuse filesystem itself */
  id = xdp_fuse_lookup_id_for_inode (st_buf->st_ino);
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

  return g_steal_pointer (&id);
}

static void
portal_add (GDBusMethodInvocation *invocation,
            GVariant              *parameters,
            XdpAppInfo            *app_info)
{
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autofree char *id = NULL;
  int fd_id, fd, fds_len;
  g_autofree char *path = NULL;
  const int *fds;
  struct stat st_buf, real_parent_st_buf;
  gboolean reuse_existing, persistent;
  GError *error = NULL;
  const char *app_id = xdp_app_info_get_id (app_info);
  int access_mode;

  g_variant_get (parameters, "(hbb)", &fd_id, &reuse_existing, &persistent);

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (fd_id < fds_len)
        fd = fds[fd_id];
    }

  if (!validate_fd (fd, app_info, &st_buf, &real_parent_st_buf, &path, &access_mode, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return;
    }

  if (st_buf.st_dev == fuse_dev)
    {
      /* The passed in fd is on the fuse filesystem itself */
      id = verify_existing_document (&st_buf, reuse_existing);
      if (id == NULL)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 "Invalid fd passed");
          return;
        }
    }
  else
    {
      {
        XDP_AUTOLOCK (db);

        id = do_create_doc (&real_parent_st_buf, path, reuse_existing, persistent);

        if (app_id[0] != '\0')
          {
            g_autoptr(PermissionDbEntry) entry = permission_db_lookup (db, id);
            DocumentPermissionFlags perms =
              DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS |
              DOCUMENT_PERMISSION_FLAGS_READ;

            if (access_mode & W_OK)
              perms |= DOCUMENT_PERMISSION_FLAGS_WRITE;

            /* If its a unique one its safe for the creator to
               delete it at will */
            if (!reuse_existing)
              perms |= DOCUMENT_PERMISSION_FLAGS_DELETE;

            do_set_permissions (entry, id, app_id, perms);
          }
      }

      /* Invalidate with lock dropped to avoid deadlock */
      xdp_fuse_invalidate_doc_app (id, NULL);
      if (app_id[0] != '\0')
        xdp_fuse_invalidate_doc_app (id, app_id);
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id));
}

static char *
flatpak (GError **error,
         ...)
{
  gboolean res;
  g_autofree char *output = NULL;
  va_list ap;

  va_start (ap, error);
  res = xdp_spawn (NULL, &output, 0, error, "flatpak", ap);
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
  g_autoptr(GKeyFile) keyfile = NULL;
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

  /* First we try flatpak info --file-access=PATH APPID, which is supported on new versions */
  arg = g_strdup_printf ("--file-access=%s", path);
  res = flatpak (&error, "info", arg, target_app_id, NULL);

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
  const char *app_id = xdp_app_info_get_id (app_info);
  GDBusMessage *message;
  GUnixFDList *fd_list;
  char *id;
  int fd_id, fd, fds_len;
  const int *fds = NULL;
  struct stat st_buf;
  gboolean reuse_existing, persistent, as_needed_by_app;
  GError *error = NULL;
  guint32 flags = 0;
  g_autoptr(GVariant) array = NULL;
  const char *target_app_id;
  g_autofree const char **permissions = NULL;
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_autofree int *access_modes = NULL;
  g_autofree struct stat *real_parent_st_bufs = NULL;
  int i;
  gsize n_args;
  DocumentPermissionFlags target_perms;
  GVariantBuilder builder;

  g_variant_get (parameters, "(@ahus^a&s)",
                 &array, &flags, &target_app_id, &permissions);

  if ((flags & ~DOCUMENT_ADD_FLAGS_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid flags");
      return;
    }

  reuse_existing = (flags & DOCUMENT_ADD_FLAGS_REUSE_EXISTING) != 0;
  persistent = (flags & DOCUMENT_ADD_FLAGS_PERSISTENT) != 0;
  as_needed_by_app = (flags & DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP) != 0;

  target_perms = xdp_parse_permissions (permissions);

  n_args = g_variant_n_children (array);
  g_ptr_array_set_size (ids, n_args + 1);
  g_ptr_array_set_size (paths, n_args + 1);
  real_parent_st_bufs = g_new0 (struct stat, n_args);
  access_modes = g_new0 (int, n_args);

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);
  if (fd_list != NULL)
    fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);

  for (i = 0; i < n_args; i++)
    {
      g_autofree char *path = NULL;

      g_variant_get_child (array, i, "h", &fd_id);

      fd = -1;
      if (fds != NULL && fd_id < fds_len)
        fd = fds[fd_id];

      if (!validate_fd (fd, app_info, &st_buf, &real_parent_st_bufs[i], &path, &access_modes[i], &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          return;
        }

      if ((target_perms & DOCUMENT_PERMISSION_FLAGS_WRITE) && (access_modes[i] & W_OK) == 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                                 "Not enough permissions");
          return;
        }

      g_ptr_array_index(paths,i) = g_steal_pointer (&path);

      if (st_buf.st_dev == fuse_dev)
        {
          /* The passed in fd is on the fuse filesystem itself */
          id = verify_existing_document (&st_buf, reuse_existing);
          if (id == NULL)
            {
              g_dbus_method_invocation_return_error (invocation,
                                                     XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                     "Invalid fd passed");
              return;
            }
          g_ptr_array_index(ids,i) = id;
        }
    }

  {
    DocumentPermissionFlags caller_base_perms =
      DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS |
      DOCUMENT_PERMISSION_FLAGS_READ;

    /* If its a unique one its safe for the creator to
       delete it at will */
    if (!reuse_existing)
      caller_base_perms |= DOCUMENT_PERMISSION_FLAGS_DELETE;

    XDP_AUTOLOCK (db); /* Lock once for all ops */

    for (i = 0; i < n_args; i++)
      {
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
            id = do_create_doc (&real_parent_st_bufs[i], path, reuse_existing, persistent);
            g_ptr_array_index(ids,i) = id;

            if (app_id[0] != '\0' && strcmp (app_id, target_app_id) != 0)
              {
                DocumentPermissionFlags caller_perms = caller_base_perms;

                if (access_modes[i] & W_OK)
                  caller_perms |= DOCUMENT_PERMISSION_FLAGS_WRITE;

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
      id = g_ptr_array_index (ids,i);
      g_assert (id != NULL);

      if (*id == 0)
        continue;

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
                                         g_variant_new ("(^as@a{sv})",
                                                        (char **)ids->pdata,
                                                        g_variant_builder_end (&builder)));
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
  GError *error = NULL;
  guint32 flags = 0;
  const char *filename;
  const char *target_app_id;
  g_autofree const char **permissions = NULL;
  g_autofree char *id = NULL;
  g_autofree char *path = NULL;
  DocumentPermissionFlags target_perms;
  GVariantBuilder builder;
  int access_mode;
  g_autoptr(GVariant) filename_v = NULL;

  g_variant_get (parameters, "(h@ayus^a&s)", &parent_fd_id, &filename_v, &flags, &target_app_id, &permissions);
  filename = g_variant_get_bytestring (filename_v);

  /* This is only allowed from the host, or else we could leak existence of files */
  if (*app_id != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  if ((flags & ~DOCUMENT_ADD_FLAGS_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid flags");
      return;
    }

  reuse_existing = (flags & DOCUMENT_ADD_FLAGS_REUSE_EXISTING) != 0;
  persistent = (flags & DOCUMENT_ADD_FLAGS_PERSISTENT) != 0;
  as_needed_by_app = (flags & DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP) != 0;

  target_perms = xdp_parse_permissions (permissions);

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  parent_fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (parent_fd_id < fds_len)
        parent_fd = fds[parent_fd_id];
    }

  if (strchr (filename, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid filename passed");
      return;
    }

  if (!validate_fd_common (parent_fd, app_info, &parent_st_buf, S_IFDIR, &parent_path, &access_mode, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  if (parent_st_buf.st_dev == fuse_dev)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  if ((access_mode & W_OK) == 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
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
        id = do_create_doc (&parent_st_buf, path, reuse_existing, persistent);

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
  const char *app_id = xdp_app_info_get_id (app_info);
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
  g_autoptr(GError) error = NULL;
  int access_mode;

  g_autoptr(GVariant) filename_v = NULL;

  g_variant_get (parameters, "(h@aybb)", &parent_fd_id, &filename_v, &reuse_existing, &persistent);
  filename = g_variant_get_bytestring (filename_v);

  /* This is only allowed from the host, or else we could leak existence of files */
  if (*app_id != 0)
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

  if (strchr (filename, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid filename passed");
      return;
    }

  if (!validate_fd_common (parent_fd, app_info, &parent_st_buf, S_IFDIR, &parent_path, &access_mode, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  if (parent_st_buf.st_dev == fuse_dev)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  if ((access_mode & W_OK) == 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  path = g_build_filename (parent_path, filename, NULL);

  g_debug ("portal_add_named %s", path);

  XDP_AUTOLOCK (db);

  id = do_create_doc (&parent_st_buf, path, reuse_existing, persistent);

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
  struct stat st_buf, real_parent_st_buf;
  g_auto(GStrv) ids = NULL;
  g_autofree char *id = NULL;
  GError *error = NULL;

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
      g_set_error_literal (&error, G_IO_ERROR,
                           g_io_error_from_errno (errsv),
                           g_strerror (errsv));
      g_dbus_method_invocation_take_error (invocation, error);
      return TRUE;
    }

  if (!validate_fd (fd, app_info, &st_buf, &real_parent_st_buf, &path, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return TRUE;
    }

  if (st_buf.st_dev == fuse_dev)
    {
      /* The passed in fd is on the fuse filesystem itself */
      id = xdp_fuse_lookup_id_for_inode (st_buf.st_ino);
      g_debug ("path on fuse, id %s", id);
    }
  else
    {
      g_autoptr(GVariant) data = NULL;

      data = g_variant_ref_sink (g_variant_new ("(^ayttu)",
                                                path,
                                                (guint64)real_parent_st_buf.st_dev,
                                                (guint64)real_parent_st_buf.st_ino,
                                                0));
      ids = permission_db_list_ids_by_value (db, data);
      if (ids[0] != NULL)
        id = g_strdup (ids[0]);
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

  g_variant_get (data, "(^ayttu)", &path, NULL, NULL, NULL);
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
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  dbus_api = xdp_dbus_documents_skeleton_new ();

  xdp_dbus_documents_set_version (XDP_DBUS_DOCUMENTS (dbus_api), 3);

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

  xdp_connection_track_name_owners (connection, NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (dbus_api),
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }
}

static void
daemon_report_done (int status)
{
  if (daemon_event_fd != -1)
    {
      guint64 counter;

      counter = status + 1;
      if (write (daemon_event_fd, &counter, sizeof (counter)) < 0)
          g_critical ("Unable to report exit status: %s", g_strerror (errno));

      daemon_event_fd = -1;
    }
}

static void
do_exit (int status)
{
  daemon_report_done (status);
  exit (status);
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

  while ((invocation = g_queue_pop_head (&get_mount_point_invocations)) != NULL)
    {
      xdp_dbus_documents_complete_get_mount_point (dbus_api, invocation, xdp_fuse_get_mountpoint ());
      g_object_unref (invocation);
    }

  daemon_report_done (0);
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
static gboolean opt_daemon;
static gboolean opt_replace;
static gboolean opt_version;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information", NULL },
  { "daemon", 'd', 0, G_OPTION_ARG_NONE, &opt_daemon, "Run in background", NULL },
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
  GOptionContext *context;
  GDBusMethodInvocation *invocation;

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

  if (opt_daemon)
    {
      pid_t pid;
      ssize_t read_res;

      daemon_event_fd = eventfd (0, EFD_CLOEXEC);
      pid = fork ();
      if (pid != 0)
        {
          guint64 counter;

          read_res = read (daemon_event_fd, &counter, sizeof (counter));
          if (read_res != 8)
            exit (1);
          exit (counter - 1);
        }
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  loop = g_main_loop_new (NULL, FALSE);

  path = g_build_filename (g_get_user_data_dir (), "flatpak/db", TABLE_NAME, NULL);
  db = permission_db_new (path, FALSE, &error);
  if (db == NULL)
    {
      g_printerr ("Failed to load db: %s", error->message);
      do_exit (2);
    }

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      do_exit (3);
    }

  permission_store = xdg_permission_store_proxy_new_sync (session_bus, G_DBUS_PROXY_FLAGS_NONE,
                                                          "org.freedesktop.impl.portal.PermissionStore",
                                                          "/org/freedesktop/impl/portal/PermissionStore",
                                                          NULL, &error);
  if (permission_store == NULL)
    {
      g_print ("No permission store: %s", error->message);
      do_exit (4);
    }

  /* We want do do our custom post-mainloop exit */
  g_dbus_connection_set_exit_on_close (session_bus, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed), NULL);

  if (set_one_signal_handler (SIGHUP, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGINT, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGTERM, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGPIPE, SIG_IGN, 0) == -1)
    do_exit (5);

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

  do_exit (final_exit_status);

  return 0;
}
