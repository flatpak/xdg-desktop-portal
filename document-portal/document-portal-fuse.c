/*
 * Copyright © 2018 Red Hat, Inc
 * Copyright © 2023 GNOME Foundation Inc.
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
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Hubert Figuière <hub@figuiere.net>
 */

#include "config.h"

#define FUSE_USE_VERSION 35

#include <glib-unix.h>

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <pthread.h>
#if HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif
#include <sys/types.h>
#if HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#if HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif
#if HAVE_SYS_EXTATTR_H
#include <sys/extattr.h>
#endif
#include <sys/time.h>
#include <sys/resource.h>

#include "document-portal-fuse.h"
#include "document-store.h"
#include "src/xdp-utils.h"

#ifndef O_FSYNC
#define O_FSYNC O_SYNC
#endif

#ifndef ENODATA
#define ENODATA ENOATTR
#endif

#define XDP_XATTR_HOST_PATH "user.document-portal.host-path"
/* man listxattr:
 *   The list is the set of (null-terminated) names, one after the other.
 *
 * Example:
 *   user.name1\0system.name1\0user.name2\0
 *
 * We can statically define the string by doing this:
 *   XDP_XATTR_MY_ATTR "\0" XDP_XATTR_ANOTHER_ATTR
 */
#define XDP_XATTR_ATTRIBUTE_NAME_LIST XDP_XATTR_HOST_PATH

/* Inode ownership model
 *
 * The document portal exposes something as a filesystem that it
 * doesn't have full control over. For instance at any point some
 * other process can rename an exposed file on the real filesystem and
 * we won't be told about this. This means that in general we always
 * return 0 for the cacheable lifetimes of entries and file attributes.
 * (Except for the virtual directories we have full control of, the
 * section below only discusses real files).
 *
 * However, even though we don't have full control of the underlying
 * filesystem the *kernel* has. This means we can use that to get
 * the correct semantics.
 *
 * For example, assume that a directory is held open by a process
 * (for example, it could be the CWD of the process). When we open
 * the directory via a LOOKUP operation we return an inode to it,
 * and, for as long as the kernel has this inode around (i.e.  until it
 * sent a FORGET message), it can send operations on this inode without
 * looking it up again. For example, if the above process used a
 * relative path.
 *
 * Now, consider the case where the app chdir():ed into the fuse
 * directory, but after the backing directory was renamed outside
 * the fuse filesystem. The fuse inode representation for the inode
 * cannot be the directory name, because the expected semantics is
 * that further relative pathnames from the app will still resolve
 * to the same directory independent of its location in the tree.
 *
 * The way we do this is to keep a O_PATH file descriptor around for
 * each underlying inode. This is represented by the XdpPhysicalInode
 * type and we have a hashtable from backing dev+inode to a these
 * so that we can use one fd per backing inode even when the file
 * is visible in many places.
 *
 * Since we don't do any caching, each LOOKUP operation will do a
 * statat() on the underlying filesystem. However, we then use the
 * result of that to lookup (via backing dev+ino) the previous inode
 * (as long as it still lives) if the backing file was unchanged.
 *
 * One problem with this approach is that the kernel tends to keep
 * inodes alive for a very long time even if they are *only*
 * referenced by the dcache (Directory Entry Cache) (even though we
 * will not really use the dcache info due to the 0 valid time). This
 * is unfortunate, because it means we will keep a lot of file
 * descriptors open. But, we can not know if the kernel needs the inode
 * for some non-dcache use so we can't close the file descriptors.
 *
 * To work around this we regularly emit entry invalidation calls
 * to the kernel, which will make it forget the inodes that are
 * only pinned by the dcache.
 */


#define NON_DOC_DIR_PERMS 0500
#define DOC_DIR_PERMS_FILE 0700
#define DOC_DIR_PERMS_DIR 0500

static GThread *fuse_thread = NULL;
static struct fuse_session *session = NULL;
G_LOCK_DEFINE (session);
static char *mount_path = NULL;
static pthread_t fuse_pthread = 0;
static uid_t my_uid;
static gid_t my_gid;

static GList *open_files = NULL;
G_LOCK_DEFINE (open_files);

/* from libfuse */
#define FUSE_UNKNOWN_INO 0xffffffff

#define BY_APP_NAME "by-app"

typedef struct {
  ino_t ino;
  dev_t dev;
} DevIno;

typedef enum {
 XDP_DOMAIN_ROOT,
 XDP_DOMAIN_BY_APP,
 XDP_DOMAIN_APP,
 XDP_DOMAIN_DOCUMENT,
} XdpDomainType;

typedef struct _XdpDomain XdpDomain;
typedef struct _XdpInode XdpInode;

struct _XdpDomain {
  gint ref_count; /* atomic */
  XdpDomainType type;

  XdpDomain *parent;
  XdpInode *parent_inode; /* Inode of the parent domain (NULL for root) */

  char *doc_id; /* NULL for root, by-app, app */
  char *app_id; /* NULL for root, by-app, non-app id */

  /* root: by docid
   * app: by docid
   * by_app: by app
   * document: by physical
   */
  GHashTable *inodes; /* Protected by domain_inodes */

  /* Below only used for XDP_DOMAIN_DOCUMENT */

  char *doc_path; /* path to the directory the files are in */
  char *doc_file; /* != NULL for non-directory documents */
  guint64 doc_dir_device;
  guint64 doc_dir_inode;
  guint32 doc_flags;

  /* Below is mutable, protected by mutex */
  GMutex  tempfile_mutex;
  GHashTable *tempfiles; /* Name -> physical */
};

static void xdp_domain_unref (XdpDomain *domain);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpDomain, xdp_domain_unref)

G_LOCK_DEFINE (domain_inodes);

typedef struct {
  gint ref_count; /* atomic */
  DevIno backing_devino;
  int fd; /* O_PATH fd */
} XdpPhysicalInode;

static XdpPhysicalInode *xdp_physical_inode_ref   (XdpPhysicalInode *inode);
static void              xdp_physical_inode_unref (XdpPhysicalInode *inode);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpPhysicalInode, xdp_physical_inode_unref)

typedef struct {
  gint ref_count; /* atomic */

  char *name;      /* This changes over time (i.e. in renames)
                      protected by domain->tempfile_mutex,
                      used as key in domain->tempfiles */
  char *tempname;  /* Real filename on disk.
                      This can be NULLed to avoid unlink at finalize */
  XdpInode *inode;
} XdpTempfile;

static XdpTempfile *xdp_tempfile_ref   (XdpTempfile *tempfile);
static void         xdp_tempfile_unref (XdpTempfile *tempfile);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpTempfile, xdp_tempfile_unref)

struct _XdpInode {
  guint64 ino;
  gint ref_count; /* atomic, includes one ref if kernel_ref_count != 0 */
  gint kernel_ref_count; /* atomic */

  XdpDomain *domain;

  /* The below are only used for XDP_DOMAIN_DOCUMENT inodes */
  XdpPhysicalInode *physical;
  /* The root of the domain, or NULL for the domain.  We use this to
   * keep the root document inode alive so that when the kernel
   * forgets it and then looks it up we will not get a new inode and
   * thus a new domain. */
  XdpInode *domain_root_inode;
};

typedef struct {
  int fd;
  GList *link;
} XdpFile;


typedef struct {
  DIR *dir;
  struct dirent *entry;
  off_t offset;

  char *dirbuf;
  gsize dirbuf_size;
} XdpDir;

XdpInode *root_inode;
XdpInode *by_app_inode;

typedef struct {
  ino_t parent_ino;
  char name[0];
} XdpInvalidateData;

typedef struct {
  gboolean use_splice;
} XdpFuseOptions;

static GList *invalidate_list;
G_LOCK_DEFINE (invalidate_list);

static XdpInode *xdp_inode_ref (XdpInode *inode);
static void xdp_inode_unref (XdpInode *inode);

/* Lookup by inode for verification */
static GHashTable *all_inodes; /* guint64 -> XdpInode */
static guint64 next_virtual_inode = FUSE_ROOT_ID; /* root is the first inode created, so it gets this */
G_LOCK_DEFINE (all_inodes);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpInode, xdp_inode_unref)

static int ensure_docdir_inode (XdpInode *parent,
                                int o_path_fd_in, /* Takes ownership */
                                struct fuse_entry_param *e,
                                XdpInode **inode_out);

static void queue_invalidate_dentry (XdpInode *parent, const char *name);

static gboolean
app_can_write_doc (PermissionDbEntry *entry, const char *app_id)
{
  if (app_id == NULL)
    return TRUE;

  if (document_entry_has_permissions_by_app_id (entry, app_id, DOCUMENT_PERMISSION_FLAGS_WRITE))
    return TRUE;

  return FALSE;
}

static gboolean
app_can_see_doc (PermissionDbEntry *entry, const char *app_id)
{
  if (app_id == NULL)
    return TRUE;

  if (document_entry_has_permissions_by_app_id (entry, app_id, DOCUMENT_PERMISSION_FLAGS_READ))
    return TRUE;

  return FALSE;
}

static char *
fd_to_path (int fd)
{
  return g_strdup_printf ("/proc/self/fd/%d", fd);
}

static char *
open_flags_to_string (int flags)
{
  GString *s;
  const char *mode;

  switch (flags & O_ACCMODE)
    {
    case O_RDONLY:
      mode = "RDONLY";
      break;
    case O_WRONLY:
      mode = "WRONLY";
      break;
    case O_RDWR:
    default:
      mode = "RDWR";
      break;
    }

  s = g_string_new (mode);

  if (flags & O_NONBLOCK)
    g_string_append (s, ",NONBLOCK");
  if (flags & O_APPEND)
    g_string_append (s, ",APPEND");
  if (flags & O_SYNC)
    g_string_append (s, ",SYNC");
  if (flags & O_ASYNC)
    g_string_append (s, ",ASYNC");
  if (flags & O_FSYNC)
    g_string_append (s, ",FSYNC");
#ifdef O_DSYNC
  if (flags & O_DSYNC)
    g_string_append (s, ",DSYNC");
#endif
  if (flags & O_CREAT)
    g_string_append (s, ",CREAT");
  if (flags & O_TRUNC)
    g_string_append (s, ",TRUNC");
  if (flags & O_EXCL)
    g_string_append (s, ",EXCL");
  if (flags & O_CLOEXEC)
    g_string_append (s, ",CLOEXEC");
  if (flags & O_DIRECT)
    g_string_append (s, ",DIRECT");
#ifdef O_LARGEFILE
  if (flags & O_LARGEFILE)
    g_string_append (s, ",LARGEFILE");
#endif
#ifdef O_NOATIME
  if (flags & O_NOATIME)
    g_string_append (s, ",NOATIME");
#endif
  if (flags & O_NOCTTY)
    g_string_append (s, ",NOCTTY");
  if (flags & O_PATH)
    g_string_append (s, ",PATH");
#ifdef O_TMPFILE
  if (flags & O_TMPFILE)
    g_string_append (s, ",TMPFILE");
#endif

  return g_string_free (s, FALSE);
}


static char *
setattr_flags_to_string (int flags)
{
  GString *s = g_string_new ("");

  if (flags & FUSE_SET_ATTR_MODE)
    g_string_append (s, "MODE,");

  if (flags & FUSE_SET_ATTR_UID)
    g_string_append (s, "UID,");

  if (flags & FUSE_SET_ATTR_GID)
    g_string_append (s, "GID,");

  if (flags & FUSE_SET_ATTR_SIZE)
    g_string_append (s, "SIZE,");

  if (flags & FUSE_SET_ATTR_ATIME)
    g_string_append (s, "ATIME,");

  if (flags & FUSE_SET_ATTR_MTIME)
    g_string_append (s, "MTIME,");

  if (flags & FUSE_SET_ATTR_ATIME_NOW)
    g_string_append (s, "ATIME_NOW,");

  if (flags & FUSE_SET_ATTR_MTIME_NOW)
    g_string_append (s, "MTIME_NOW,");

  /* Remove last comma */
  if (s->len > 0)
    g_string_truncate (s, s->len - 1);

  return g_string_free (s, FALSE);
}

static char *
renameat2_flags_to_string (int flags)
{
#if HAVE_RENAMEAT2
  GString *s = g_string_new ("");

  if (flags & RENAME_EXCHANGE)
    g_string_append (s, "EXCHANGE,");

  if (flags & RENAME_NOREPLACE)
    g_string_append (s, "NOREPLACE,");

  if (flags & RENAME_WHITEOUT)
    g_string_append (s, "WHITEOUT,");

  /* Remove last comma */
  if (s->len > 0)
    g_string_truncate (s, s->len - 1);

  return g_string_free (s, FALSE);
#else
  return g_strdup_printf ("%#x", flags);
#endif
}

static guint
devino_hash  (gconstpointer  key)
{
  DevIno *devino = (DevIno *)key;

  return (devino->ino >> 2) ^ devino->dev;
}

static gboolean
devino_equal (gconstpointer  _a,
              gconstpointer  _b)
{
  DevIno *a = (DevIno *)_a;
  DevIno *b = (DevIno *)_b;
  return a->ino == b->ino && a->dev == b->dev;
}

/* Lookup by physical backing devino */
static GHashTable *physical_inodes;
G_LOCK_DEFINE (physical_inodes);


/* Takes ownership of the o_path fd if passed in */
static XdpPhysicalInode *
ensure_physical_inode (dev_t dev, ino_t ino, int o_path_fd)
{
  DevIno devino = {ino, dev};
  XdpPhysicalInode *inode = NULL;

  G_LOCK (physical_inodes);

  inode = g_hash_table_lookup (physical_inodes, &devino);
  if (inode != NULL)
    {
      inode = xdp_physical_inode_ref (inode);
      close (o_path_fd);
    }
  else
    {
      /* Takes ownership of fd */
      inode = g_new0 (XdpPhysicalInode, 1);
      inode->ref_count = 1;
      inode->fd = o_path_fd;
      inode->backing_devino = devino;
      g_hash_table_insert (physical_inodes, &inode->backing_devino, inode);
    }

  G_UNLOCK (physical_inodes);

  return inode;
}

static XdpPhysicalInode *
xdp_physical_inode_ref (XdpPhysicalInode *inode)
{
  g_atomic_int_inc (&inode->ref_count);
  return inode;
}

static void
xdp_physical_inode_unref (XdpPhysicalInode *inode)
{
  gint old_ref;

  /* here we want to atomically do: if (ref_count>1) { ref_count--; return; } */
retry_atomic_decrement1:
  old_ref = g_atomic_int_get (&inode->ref_count);
  if (old_ref > 1)
    {
      if (!g_atomic_int_compare_and_exchange ((int *) &inode->ref_count, old_ref, old_ref - 1))
        goto retry_atomic_decrement1;
    }
  else
    {
      if (old_ref <= 0)
        {
          g_warning ("Can't unref dead inode");
          return;
        }

      /* Might be revived from physical_inodes hash by this time, so protect by lock */
      G_LOCK (physical_inodes);

      if (!g_atomic_int_compare_and_exchange ((int *) &inode->ref_count, old_ref, old_ref - 1))
        {
          G_UNLOCK (physical_inodes);
          goto retry_atomic_decrement1;
        }
      g_hash_table_remove (physical_inodes, &inode->backing_devino);

      G_UNLOCK (physical_inodes);

      close (inode->fd);
      g_free (inode);
    }
}

static XdpDomain *
xdp_domain_ref (XdpDomain *domain)
{
  g_atomic_int_inc (&domain->ref_count);
  return domain;
}

static void
xdp_domain_unref (XdpDomain *domain)
{
  if (g_atomic_int_dec_and_test (&domain->ref_count))
    {
      g_free (domain->doc_id);
      g_free (domain->app_id);
      g_free (domain->doc_path);
      g_free (domain->doc_file);
      if (domain->inodes)
        g_assert (g_hash_table_size (domain->inodes) == 0);
      g_clear_pointer (&domain->inodes, g_hash_table_unref);
      g_clear_pointer (&domain->parent, xdp_domain_unref);
      g_clear_pointer (&domain->parent_inode, xdp_inode_unref);
      g_clear_pointer (&domain->tempfiles, g_hash_table_unref);
      g_mutex_clear (&domain->tempfile_mutex);
      g_free (domain);
    }
}

static gboolean
xdp_domain_is_virtual_type (XdpDomain *domain)
{
  return
    domain->type == XDP_DOMAIN_ROOT ||
    domain->type == XDP_DOMAIN_BY_APP ||
    domain->type == XDP_DOMAIN_APP;
}

static XdpDomain *
_xdp_domain_new (XdpDomainType type)
{
  XdpDomain *domain = g_new0 (XdpDomain, 1);
  domain->ref_count = 1;
  domain->type = type;
  g_mutex_init (&domain->tempfile_mutex);
  return domain;
}

static XdpDomain *
xdp_domain_new_root (void)
{
  XdpDomain *domain = _xdp_domain_new (XDP_DOMAIN_ROOT);
  domain->inodes = g_hash_table_new (g_str_hash, g_str_equal);
  return domain;
}

static XdpDomain *
xdp_domain_new_by_app (XdpInode *root_inode)
{
  XdpDomain *root_domain = root_inode->domain;
  XdpDomain *domain = _xdp_domain_new (XDP_DOMAIN_BY_APP);
  domain->parent = xdp_domain_ref (root_domain);
  domain->parent_inode = xdp_inode_ref (root_inode);
  domain->inodes = g_hash_table_new (g_str_hash, g_str_equal);
  return domain;
}

static XdpDomain *
xdp_domain_new_app (XdpInode   *parent_inode,
                    const char *app_id)
{
  XdpDomain *parent = parent_inode->domain;
  XdpDomain *domain = _xdp_domain_new (XDP_DOMAIN_APP);
  domain->parent = xdp_domain_ref (parent);
  domain->parent_inode = xdp_inode_ref (parent_inode);
  domain->app_id = g_strdup (app_id);
  domain->inodes = g_hash_table_new (g_str_hash, g_str_equal);
  return domain;
}

static gboolean
xdp_document_domain_is_dir (XdpDomain *domain)
{
  return (domain->doc_flags & DOCUMENT_ENTRY_FLAG_DIRECTORY) != 0;
}

static XdpDomain *
xdp_domain_new_document (XdpDomain         *parent,
                         const char        *doc_id,
                         PermissionDbEntry *doc_entry)
{
  XdpDomain *domain = _xdp_domain_new (XDP_DOMAIN_DOCUMENT);
  const char *db_path;

  domain->parent = xdp_domain_ref (parent);
  domain->doc_id = g_strdup (doc_id);
  domain->app_id = g_strdup (parent->app_id);
  domain->inodes = g_hash_table_new (g_direct_hash, g_direct_equal);

  domain->doc_flags = document_entry_get_flags (doc_entry);
  domain->doc_dir_device = document_entry_get_device (doc_entry);
  domain->doc_dir_inode =  document_entry_get_inode (doc_entry);

  db_path = document_entry_get_path (doc_entry);
  if (xdp_document_domain_is_dir (domain))
    {
      domain->doc_path = g_strdup (db_path);
      domain->doc_file = g_path_get_basename (db_path);
    }
  else
    {
      domain->doc_path = g_path_get_dirname (db_path);
      domain->doc_file = g_path_get_basename (db_path);
    }

  domain->tempfiles = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)xdp_tempfile_unref);

  return domain;
}

static gboolean
xdp_document_domain_can_see (XdpDomain *domain)
{
  if (domain->app_id != NULL)
    {
      g_autoptr(PermissionDbEntry) entry = xdp_lookup_doc (domain->doc_id);

      if (entry == NULL ||
          !app_can_see_doc (entry, domain->app_id))
        return FALSE;
    }

  return TRUE;
}

static gboolean
xdp_document_domain_can_write (XdpDomain *domain)
{
  if (domain->app_id != NULL)
    {
      g_autoptr(PermissionDbEntry) entry = xdp_lookup_doc (domain->doc_id);

      if (entry == NULL ||
          !app_can_write_doc (entry, domain->app_id))
        return FALSE;
    }

  return TRUE;
}

static char **
xdp_domain_get_inode_keys_as_string (XdpDomain *domain)
{
  char **res;
  guint length, i;

  g_assert (domain->type == XDP_DOMAIN_BY_APP);

  G_LOCK (domain_inodes);

  res = (char **)g_hash_table_get_keys_as_array (domain->inodes, &length);
  for (i = 0; i < length; i++)
    res[i] = g_strdup (res[i]);

  G_UNLOCK (domain_inodes);

  return res;
}

static XdpTempfile *
xdp_tempfile_ref (XdpTempfile *tempfile)
{
  g_atomic_int_inc (&tempfile->ref_count);
  return tempfile;
}

static XdpTempfile *
xdp_tempfile_new (XdpInode   *inode,
                  const char *name,
                  const char *tempname)
{
  XdpTempfile *tempfile = g_new0 (XdpTempfile, 1);

  tempfile->ref_count = 1;
  tempfile->inode = xdp_inode_ref (inode);
  tempfile->name = g_strdup (name);
  tempfile->tempname = g_strdup (tempname);

  return tempfile;
}

static void
xdp_tempfile_unref (XdpTempfile *tempfile)
{
  if (g_atomic_int_dec_and_test (&tempfile->ref_count))
    {
      if (tempfile->tempname)
        {
          g_autofree char *temppath = g_build_filename (tempfile->inode->domain->doc_path, tempfile->tempname, NULL);
          (void)unlink (temppath);
        }
      g_free (tempfile->name);
      g_free (tempfile->tempname);
      g_clear_pointer (&tempfile->inode, xdp_inode_unref);
      g_free (tempfile);
    }
}

static XdpInode *
_xdp_inode_new (void)
{
  XdpInode *inode = g_new0 (XdpInode, 1);
  inode->ref_count = 1;
  inode->kernel_ref_count = 0;

  return inode;
}

/* We try to create persistent inode number based on the backing
 * device and inode numbers, as well as the doc/app id (since the same
 * backing dev/ino should be different inodes in the fuse
 * filesystem). We do this by hashing the data to generate a value.
 * For non-physical files or accidental collisions we just pick a free
 * number by incrementing.
 */
static guint64
generate_persistent_ino (DevIno *backing_devino,
                         const char *doc_id,
                         const char *app_id)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_MD5);
  guchar digest[64];
  gsize digest_len = 64;
  guint64 res;

  g_checksum_update (checksum, (guchar *)backing_devino, sizeof (DevIno));
  if (doc_id)
    g_checksum_update (checksum, (guchar *)doc_id, strlen (doc_id));
  if (app_id)
    g_checksum_update (checksum, (guchar *)app_id, strlen (app_id));

  g_checksum_get_digest (checksum, digest, &digest_len);

  res = *(guint64 *)digest;
  if (res == FUSE_ROOT_ID || res == 0)
    res = FUSE_ROOT_ID + 1;
  return res;
}

/* takes ownership of fd */
static XdpInode *
xdp_inode_new (XdpDomain        *domain,
               XdpPhysicalInode *physical)
{
  XdpInode *inode = _xdp_inode_new ();
  inode->domain = xdp_domain_ref (domain);
  guint64 persistent_ino, try_ino;

  if (physical)
    {
      inode->physical = xdp_physical_inode_ref (physical);
      persistent_ino = generate_persistent_ino (&physical->backing_devino,
                                                domain->doc_id,
                                                domain->app_id);

    }

  G_LOCK (all_inodes);
  if (physical)
    try_ino = persistent_ino;
  else
    try_ino = next_virtual_inode++;

  if (g_hash_table_contains (all_inodes, &try_ino))
    try_ino++;

  inode->ino = try_ino;
  g_hash_table_insert (all_inodes, &inode->ino, inode);
  G_UNLOCK (all_inodes);

  return inode;
}

static ino_t
xdp_inode_to_ino (XdpInode *inode)
{
  return inode->ino;
}

/* This is called on kernel upcalls, so it *should* be guaranteed that the inode exists due to the kernel refs */
static XdpInode *
xdp_inode_from_ino (ino_t ino)
{
  XdpInode *inode;

  G_LOCK (all_inodes);
  inode = g_hash_table_lookup (all_inodes, &ino);
  G_UNLOCK (all_inodes);

  g_assert (inode != NULL);

  /* Its safe to ref it here because we know it exists outside the lock due to the kernel refs */
  return xdp_inode_ref (inode);
}

static XdpInode *
xdp_inode_ref (XdpInode *inode)
{
  g_atomic_int_inc (&inode->ref_count);
  return inode;
}

static void
xdp_inode_unref (XdpInode *inode)
{
  gint old_ref;
  XdpDomain *domain;

  /* here we want to atomically do: if (ref_count>1) { ref_count--; return; } */
retry_atomic_decrement1:
  old_ref = g_atomic_int_get (&inode->ref_count);
  if (old_ref > 1)
    {
      if (!g_atomic_int_compare_and_exchange ((int *) &inode->ref_count, old_ref, old_ref - 1))
        goto retry_atomic_decrement1;
    }
  else
    {
      if (old_ref <= 0)
        {
          g_warning ("Can't unref dead inode");
          return;
        }

      /* Might be revived from domain->inodes hash by this time, so protect by lock */
      G_LOCK (domain_inodes);

      if (!g_atomic_int_compare_and_exchange ((int *) &inode->ref_count, old_ref, old_ref - 1))
        {
          G_UNLOCK (domain_inodes);
          goto retry_atomic_decrement1;
        }

      domain = inode->domain;

      if (domain->type == XDP_DOMAIN_APP)
        g_hash_table_remove (domain->parent->inodes, domain->app_id);
      else if (domain->type == XDP_DOMAIN_DOCUMENT)
        {
          if (inode->physical)
            {
              g_hash_table_remove (domain->inodes, inode->physical);
            }
          else
            g_hash_table_remove (domain->parent->inodes, domain->doc_id);
        }

      /* Run this under domain_inodes lock to avoid race condition in ensure_docdir_inode + xdp_inode_new
       * where we don't want a domain->inode lookup to fail, but then an all_inode lookup to succeed
       * when looking for an ino collision
       *
       * Note: After the domain->inodes removal and here we don't allow resurrection, but we may
       * still race with an all_inodes lookup (e.g. in xdp_fuse_lookup_id_for_inode), which *is*
       * allowed and it can read the inode fields (while the lock is held) as they are still valid.
       **/
      G_LOCK (all_inodes);
      g_hash_table_remove (all_inodes, &inode->ino);
      G_UNLOCK (all_inodes);

      G_UNLOCK (domain_inodes);

      /* By now we have no refs outstanding and no way to get at the inode, so free it */

      g_clear_pointer (&inode->domain_root_inode, xdp_inode_unref);
      g_clear_pointer (&inode->physical, xdp_physical_inode_unref);
      xdp_domain_unref (inode->domain);
      g_free (inode);
    }

}

static XdpInode *
xdp_inode_kernel_ref (XdpInode *inode)
{
  int old;

  old = g_atomic_int_add (&inode->kernel_ref_count, 1);

  if (old == 0)
    xdp_inode_ref (inode);
  return inode;
}

static void
xdp_inode_kernel_unref (XdpInode *inode, unsigned long count)
{
  gint old_ref, new_ref;

 retry_atomic_decrement1:
  old_ref = g_atomic_int_get (&inode->kernel_ref_count);
  if (old_ref < count)
    {
      g_warning ("Can't kernel_unref inode with no kernel refs");
      return;
    }
  new_ref = old_ref - count;
  if (!g_atomic_int_compare_and_exchange (&inode->kernel_ref_count, old_ref, new_ref))
    goto retry_atomic_decrement1;

  if (new_ref == 0)
    xdp_inode_unref (inode);
}

static int
verify_doc_dir_devino (int dirfd, XdpDomain *doc_domain)
{
  struct stat buf;

  if (fstat (dirfd, &buf) != 0)
    return -errno;

  if (buf.st_ino != doc_domain->doc_dir_inode ||
      buf.st_dev != doc_domain->doc_dir_device)
    return -ENOENT;

  return 0;
}

/* Only for toplevel dirs, not this is a bit weird for toplevel dir
   inodes as it returns the dir itself which isn't really the dirfd
   for that (nonphysical) inode */
static int
xdp_nonphysical_document_inode_opendir (XdpInode *inode)
{
  XdpDomain *domain = inode->domain;
  g_autofd int dirfd = -1;
  int res;

  g_assert (domain->type == XDP_DOMAIN_DOCUMENT);
  g_assert (inode->physical == NULL);

  dirfd = open (domain->doc_path, O_PATH | O_DIRECTORY);
  if (dirfd < 0)
    return -errno;

  res = verify_doc_dir_devino (dirfd, domain);
  if (res != 0)
    return res;

  return g_steal_fd (&dirfd);
}

static int
xdp_document_inode_ensure_dirfd (XdpInode *inode,
                                 int      *close_fd_out)
{
  int close_fd;

  g_assert (inode->domain->type == XDP_DOMAIN_DOCUMENT);

  *close_fd_out = -1;

  if (inode->physical)
    return inode->physical->fd;
  else
    {
      if (xdp_document_domain_is_dir (inode->domain))
        {
          /* There is no dirfd for the toplevel dirs, happens for example
             if renaming into toplevel, so just return EPERM */
          return -EPERM;
        }

      close_fd = xdp_nonphysical_document_inode_opendir (inode);
      if (close_fd < 0)
        return close_fd;

      *close_fd_out = close_fd;
      return close_fd;
    }
}

static gboolean
open_flags_has_write (int open_flags)
{
  return
    (open_flags & O_ACCMODE) == O_WRONLY ||
    (open_flags & O_ACCMODE) == O_RDWR ||
    open_flags & O_TRUNC;
}

static void
gen_temp_name (gchar *tmpl)
{
  g_return_if_fail (tmpl != NULL);
  const size_t len = strlen (tmpl);
  g_return_if_fail (len >= 6);

  static const char letters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  static const int NLETTERS = sizeof (letters) - 1;

  char *XXXXXX = tmpl + (len - 6);
  for (int i = 0; i < 6; i++)
    XXXXXX[i] = letters[g_random_int_range(0, NLETTERS)];
}

static int
open_temp_at (int          dirfd,
              const char  *orig_name,
              char       **name_out,
              mode_t       mode)
{
  g_autofree char *tmp = g_strconcat (".xdp-", orig_name, "-XXXXXX", NULL);
  const size_t count_max = 100;
  int errsv;
  int fd;

  for (size_t count = 0; count < count_max; count++)
    {
      gen_temp_name (tmp);

      fd = openat (dirfd, tmp, O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_RDWR, mode);
      errsv = errno;
      if (fd < 0)
        {
          if (errsv == EEXIST)
            continue;
          else
            return -errsv;
        }

      *name_out = g_steal_pointer (&tmp);
      return fd;
    }

  return -EEXIST;
}

/* allocates tempfile for existing file,
   Called with tempfile lock held, sets errno */
static int
get_tempfile_for (XdpInode     *parent,
                  XdpDomain    *domain,
                  const char   *name,
                  int           dirfd,
                  const char   *tmpname,
                  XdpTempfile **tempfile_out)
{
  g_autoptr(XdpTempfile) tempfile = NULL;
  g_autoptr(XdpInode) inode = NULL;
  g_autofd int o_path_fd = -1;
  int res;

  if (tempfile_out != NULL)
    *tempfile_out = NULL;

  o_path_fd = openat (dirfd, tmpname, O_PATH, 0);
  if (o_path_fd == -1)
    return -errno;

  res = ensure_docdir_inode (parent, g_steal_fd (&o_path_fd), NULL, &inode); /* passed ownership of o_path_fd */
  if (res != 0)
    return res;

  tempfile = xdp_tempfile_new (inode, name, tmpname);

  /* This is atomic, because we're called with the lock held */
  g_hash_table_replace (domain->tempfiles, tempfile->name, xdp_tempfile_ref (tempfile));

  queue_invalidate_dentry (parent, tempfile->name);

  if (tempfile_out)
    *tempfile_out = g_steal_pointer (&tempfile);
  return 0;
}

/* Creates a new file on disk,
   Called with tempfile lock held, sets errno */
static int
create_tempfile (XdpInode     *parent,
                 XdpDomain    *domain,
                 const char   *name,
                 int           dirfd,
                 mode_t        mode,
                 XdpTempfile **tempfile_out)
{
  g_autoptr(XdpInode) inode = NULL;
  g_autofree char *real_fd_path = NULL;
  g_autofd int real_fd = -1;
  g_autofd int o_path_fd = -1;
  g_autoptr(XdpTempfile) tempfile = NULL;
  g_autofree char *tmpname = NULL;
  int res;

  if (tempfile_out != NULL)
    *tempfile_out = NULL;

  real_fd = open_temp_at (dirfd, name, &tmpname, mode);
  if (real_fd < 0)
    return real_fd;

  real_fd_path = fd_to_path (real_fd);
  o_path_fd = open (real_fd_path, O_PATH, 0);
  if (o_path_fd == -1)
    return -errno;

  /* We can close the tmpfd early */
  close (g_steal_fd (&real_fd));

  res = ensure_docdir_inode (parent, g_steal_fd (&o_path_fd), NULL, &inode); /* passed ownership of o_path_fd */
  if (res != 0)
    return res;

  tempfile = xdp_tempfile_new (inode, name, tmpname);

  /* This is atomic, because we're called with the lock held */
  g_hash_table_replace (domain->tempfiles, tempfile->name, xdp_tempfile_ref (tempfile));

  queue_invalidate_dentry (parent, tempfile->name);

  if (tempfile_out)
    *tempfile_out = g_steal_pointer (&tempfile);
  return 0;
}

static int
xdp_document_inode_open_child_fd (XdpInode   *inode,
                                  const char *name,
                                  int         open_flags,
                                  mode_t      mode)
{
  XdpDomain *domain = inode->domain;
  XdpTempfile *tempfile_lookup = NULL;
  g_autoptr(XdpTempfile) tempfile = NULL;
  int tempfile_res = 0;
  g_autofd int fd = -1;

  g_assert (domain->type == XDP_DOMAIN_DOCUMENT);

  if (!xdp_document_domain_can_write (domain) &&
      (open_flags_has_write (open_flags) ||
       (open_flags & O_CREAT) != 0))
      return -EACCES;

  if (inode->physical)
    {
      fd = openat (inode->physical->fd, name, open_flags, mode);
      if (fd == -1)
        return -errno;

      return g_steal_fd (&fd);
    }
  else
    {
      g_autofd int dirfd = -1;

      if (xdp_document_domain_is_dir (domain))
        {
          if (strcmp (name, domain->doc_file) == 0)
            {
              /* Ensure toplevel dir exist and is right */
              dirfd = xdp_nonphysical_document_inode_opendir (inode);
              if (dirfd < 0)
                return dirfd;

              fd = openat (dirfd, ".", open_flags, mode);
              if (fd == -1)
                return -errno;
              return g_steal_fd (&fd);
            }
        }
      else
        {
          /* Ensure parent dir exist and is right */
          dirfd = xdp_nonphysical_document_inode_opendir (inode);
          if (dirfd < 0)
            return dirfd;

          if (strcmp (name, domain->doc_file) == 0)
            {
              fd = openat (dirfd, name, open_flags, mode);
              if (fd == -1)
                return -errno;
              return g_steal_fd (&fd);
            }

          /* Not main file, maybe a temporary file? */

          g_mutex_lock (&domain->tempfile_mutex);

          tempfile_lookup = g_hash_table_lookup (domain->tempfiles, name);
          if (tempfile_lookup)
            {
              if ((open_flags & O_CREAT) && (open_flags & O_EXCL))
                tempfile_res = -EEXIST;
              else
                tempfile = xdp_tempfile_ref (tempfile_lookup);
            }
          else if (open_flags & O_CREAT)
            {
              tempfile_res = create_tempfile (inode, domain, name, dirfd, mode, &tempfile);
            }

          g_mutex_unlock (&domain->tempfile_mutex);

          if (tempfile)
            {
              g_autofree char *fd_path = fd_to_path (tempfile->inode->physical->fd);
              fd = open (fd_path, open_flags & ~(O_CREAT|O_EXCL|O_NOFOLLOW), mode);
              if (fd == -1)
                return -errno;

              return g_steal_fd (&fd);
            }
          else
            {
              if (tempfile_res != 0)
                return tempfile_res;
            }
        }
    }

  return -ENOENT;
}

/* Returns /proc/self/fds/$fd path for O_PATH fd or toplevel path */
static char *
xdp_document_inode_get_self_as_path (XdpInode *inode)
{
  g_assert (inode->domain->type == XDP_DOMAIN_DOCUMENT);

  if (inode->physical)
    return fd_to_path (inode->physical->fd);
  else
    {
      if (xdp_document_domain_is_dir (inode->domain))
        return NULL;
      return g_strdup (inode->domain->doc_path);
    }
}

static void
tweak_statbuf_for_document_inode (XdpInode    *inode,
                                  struct stat *buf)
{
  XdpDomain   *domain = inode->domain;

  g_assert (domain->type == XDP_DOMAIN_DOCUMENT);

  buf->st_ino = xdp_inode_to_ino (inode);

  /* Remove setuid/setgid/sticky flags */
  buf->st_mode &= ~(S_ISUID|S_ISGID|S_ISVTX);

  if (!xdp_document_domain_can_write (domain))
    buf->st_mode &= ~(0222);
}

static void
xdp_reply_err (const char *op,
               fuse_req_t  req,
               int         err)
{
  if (err != 0)
    {
      const char *errname = NULL;
      switch (err)
        {
        case ESTALE:
          errname = "ESTALE";
          break;
        case EEXIST:
          errname = "EEXIST";
          break;
        case ENOENT:
          errname = "ENOENT";
          break;
        case EPERM:
          errname = "EPERM";
          break;
        case EACCES:
          errname = "EACCES";
          break;
        case EINVAL:
          errname = "EINVAL";
          break;
        default:
          errname = NULL;
        }
      if (errname != NULL)
        g_debug ("%s -> error %s", op, errname);
      else
        g_debug ("%s -> error %d", op, err);
    }
  fuse_reply_err (req, err);
}

static inline void
xdp_reply_ok (const char *op,
              fuse_req_t  req)
{
  xdp_reply_err (op, req, 0);
}

typedef enum {
      CHECK_CAN_WRITE = 1 << 0,
      CHECK_IS_DIRECTORY = 1 << 1,
      CHECK_IS_PHYSICAL = 1 << 2,
      CHECK_IS_PHYSICAL_IF_DIR = 1 << 3,
} XdpDocumentChecks;

static gboolean
xdp_document_inode_checks (const char        *op,
                           fuse_req_t         req,
                           XdpInode          *inode,
                           XdpDocumentChecks  checks)
{
  XdpDomain *domain = inode->domain;
  gboolean check_is_directory = (checks & CHECK_IS_DIRECTORY) != 0;
  gboolean check_can_write = (checks & CHECK_CAN_WRITE) != 0;
  gboolean check_is_physical = (checks & CHECK_IS_PHYSICAL) != 0;
  gboolean check_is_physical_if_dir = (checks & CHECK_IS_PHYSICAL_IF_DIR) != 0;

  if (domain->type != XDP_DOMAIN_DOCUMENT)
    {
      xdp_reply_err (op, req, EPERM);
      return FALSE;
    }

  /* We allowed the inode lookup to succeed, but maybe the permissions changed since then */
  if (!xdp_document_domain_can_see (domain))
    {
      xdp_reply_err (op, req, EACCES);
      return FALSE;
    }

  if (check_is_directory && !xdp_document_domain_is_dir (domain))
    {
      xdp_reply_err (op, req, EPERM);
      return FALSE;
    }

  if (check_can_write && !xdp_document_domain_can_write (domain))
    {
      xdp_reply_err (op, req, EACCES);
      return FALSE;
    }

  if (check_is_physical_if_dir && xdp_document_domain_is_dir (domain))
    check_is_physical = TRUE;

  if (check_is_physical && inode->physical == NULL)
    {
      xdp_reply_err (op, req, EPERM);
      return FALSE;
    }

  return TRUE;
}

static void
stat_virtual_inode (XdpInode    *inode,
                    struct stat *buf)
{
  memset (buf, 0, sizeof (struct stat));
  buf->st_ino = xdp_inode_to_ino (inode);
  buf->st_uid = my_uid;
  buf->st_gid = my_gid;

  switch (inode->domain->type)
    {
    case XDP_DOMAIN_ROOT:
    case XDP_DOMAIN_BY_APP:
    case XDP_DOMAIN_APP:
      buf->st_mode = S_IFDIR | NON_DOC_DIR_PERMS;
      buf->st_nlink = 2;
      break;
    case XDP_DOMAIN_DOCUMENT:
      if (xdp_document_domain_is_dir (inode->domain))
        buf->st_mode = S_IFDIR | DOC_DIR_PERMS_DIR;
      else
        buf->st_mode = S_IFDIR | DOC_DIR_PERMS_FILE;
      buf->st_nlink = 2;

      /* Remove perms if not writable */
      if (inode->domain->app_id != NULL)
        {
          g_autoptr(PermissionDbEntry) entry = xdp_lookup_doc (inode->domain->doc_id);
          if (entry == NULL || !app_can_write_doc (entry, inode->domain->app_id))
            buf->st_mode &= ~(0222);
        }
      break;

    default:
      g_assert_not_reached ();
      break;
    }
}

static void
xdp_fuse_getattr (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  XdpDomain *domain = inode->domain;
  double attr_valid_time = 0.0;/* Time in secs for attribute validation */
  struct stat buf;
  int res;
  const char *op = "GETATTR";

  g_debug ("GETATTR %" G_GINT64_MODIFIER "x", ino);

  if (xdp_domain_is_virtual_type (domain))
    {
      stat_virtual_inode (inode, &buf);
      fuse_reply_attr (req, &buf, attr_valid_time);
      return;
    }

  g_assert (domain->type == XDP_DOMAIN_DOCUMENT);

  if (inode->physical)
    {
      res = fstatat (inode->physical->fd, "", &buf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    }
  else
    {
      stat_virtual_inode (inode, &buf);
      res = 0;
    }

  if (res == -1)
    return xdp_reply_err (op, req, errno);

  tweak_statbuf_for_document_inode (inode, &buf);

  fuse_reply_attr (req, &buf, attr_valid_time);
}

static void
xdp_fuse_setattr (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct stat           *attr,
                  int                    to_set,
                  struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  g_autofree char *to_set_string = setattr_flags_to_string (to_set);
  struct stat buf;
  double attr_valid_time = 0.0;/* Time in secs for attribute validation */
  int res;
  const char *op = "SETATTR";

  g_debug ("SETATTR %" G_GINT64_MODIFIER "x %s", ino, to_set_string);

  if (!xdp_document_inode_checks (op, req, inode,
                                  CHECK_CAN_WRITE | CHECK_IS_PHYSICAL))
    return;

  /* Truncate */
  if (to_set & FUSE_SET_ATTR_SIZE)
    {
      g_autofree char *path = NULL;
      XdpFile *file = (XdpFile *)fi->fh;

      if (file)
        {
          res = ftruncate (file->fd, attr->st_size);
          if (res == -1)
            res = -errno;
        }
      else if (inode->physical)
        {
          path = fd_to_path (inode->physical->fd);
          res = truncate (path, attr->st_size);
          if (res == -1)
            res = -errno;
        }
      else
        {
          res = -EISDIR;
        }

      if (res != 0)
        return xdp_reply_err (op, req, -res);
    }

  if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME))
    {
      struct timespec times[2] = { {0, UTIME_OMIT}, {0, UTIME_OMIT} }; /* 0 = atime, 1 = mtime */
      g_autofree char *path = NULL;

      if (to_set & FUSE_SET_ATTR_ATIME_NOW)
        times[0].tv_nsec = UTIME_NOW;
      else if (to_set & FUSE_SET_ATTR_ATIME)
        times[0] = attr->st_atim;

      if (to_set & FUSE_SET_ATTR_MTIME_NOW)
        times[1].tv_nsec = UTIME_NOW;
      else if (to_set & FUSE_SET_ATTR_MTIME)
        times[1] = attr->st_mtim;

      if (inode->physical)
        {
          path = fd_to_path (inode->physical->fd);
          res = utimensat (AT_FDCWD, path, times, 0);
        }
      else
        res = utimensat (AT_FDCWD, inode->domain->doc_path, times, 0); /* follow symlink here */

      if (res != 0)
        return xdp_reply_err (op, req, errno);
    }

  if (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID))
    {
      g_autofree char *path = NULL;
      uid_t uid = -1;
      gid_t gid = -1;

      if (to_set & FUSE_SET_ATTR_UID)
        uid = attr->st_uid;

      if (to_set & FUSE_SET_ATTR_GID)
        gid = attr->st_gid;

      if (inode->physical)
        {
          path = fd_to_path (inode->physical->fd);
          res = chown (path, uid, gid);
          if (res == -1)
            res = -errno;
        }
      else
        {
          res = -EACCES;
        }

      if (res != 0)
        return xdp_reply_err (op, req, -res);
    }

  if (to_set & (FUSE_SET_ATTR_MODE))
    {
      g_autofree char *path = NULL;

      if (inode->physical)
        {
          path = fd_to_path (inode->physical->fd);
          res = chmod (path, attr->st_mode);
          if (res == -1)
            res = -errno;
        }
      else
        {
          res = -EACCES;
        }

      if (res != 0)
        return xdp_reply_err (op, req, -res);
    }

  if (inode->physical)
    res = fstatat (inode->physical->fd, "", &buf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
  else
    res = stat (inode->domain->doc_path, &buf); /* Follow symlinks here */

  if (res != 0)
    return xdp_reply_err (op, req, errno);

  tweak_statbuf_for_document_inode (inode, &buf);

  fuse_reply_attr (req, &buf, attr_valid_time);
}

static void
prepare_reply_entry (XdpInode                *inode,
                     struct stat             *buf,
                     struct fuse_entry_param *e)
{
  xdp_inode_kernel_ref (inode); /* Ref given to the kernel, returned in xdp_forget() */
  e->ino = xdp_inode_to_ino (inode);
  e->generation = 1;
  e->attr = *buf;
  e->attr_timeout = 0.0; /* attribute timeout */
  e->entry_timeout = 0.0; /* dentry timeout */
}

static void
prepare_reply_virtual_entry (XdpInode                *inode,
                             struct fuse_entry_param *e)
{
  stat_virtual_inode (inode, &e->attr);

  xdp_inode_kernel_ref (inode); /* Ref given to the kernel, returned in xdp_forget() */
  e->ino = xdp_inode_to_ino (inode);
  e->generation = 1;

  /* Cache virtual dirs */
  e->attr_timeout = 60.0; /* attribute timeout */
  e->entry_timeout = 60.0; /* dentry timeout */
}

static void
abort_reply_entry (struct fuse_entry_param *e)
{
  XdpInode *inode = xdp_inode_from_ino (e->ino);
  xdp_inode_kernel_unref (inode, 1);
}

static int
ensure_docdir_inode (XdpInode                 *parent,
                     int                       o_path_fd_in,
                     struct fuse_entry_param  *e,
                     XdpInode                **inode_out)
{
  XdpDomain *domain = parent->domain;
  g_autoptr(XdpPhysicalInode) physical = NULL;
  g_autoptr(XdpInode) inode = NULL;
  g_autofd int o_path_fd = -1;
  struct stat buf;
  int res;

  /* Take ownership */
  o_path_fd = g_steal_fd (&o_path_fd_in);

  res = fstatat (o_path_fd, "", &buf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
  if (res == -1)
    return -errno;

  /* non-directory documents only support regular files */
  if (!xdp_document_domain_is_dir (domain) &&  !S_ISREG(buf.st_mode))
    return -ENOENT;

  physical = ensure_physical_inode (buf.st_dev, buf.st_ino, g_steal_fd (&o_path_fd)); /* passed ownership of fd */

  G_LOCK (domain_inodes);
  inode = g_hash_table_lookup (domain->inodes, physical);
  if (inode != NULL)
    inode = xdp_inode_ref (inode);
  else
    {
      inode = xdp_inode_new (domain, physical);
      if (parent->domain_root_inode)
        inode->domain_root_inode = xdp_inode_ref (parent->domain_root_inode);
      else
        inode->domain_root_inode = xdp_inode_ref (parent);
      g_hash_table_insert (domain->inodes, physical, inode);
    }
  G_UNLOCK (domain_inodes);

  if (e)
    {
      tweak_statbuf_for_document_inode (inode, &buf);
      prepare_reply_entry (inode, &buf, e);
    }

  if (inode_out)
    *inode_out = g_steal_pointer (&inode);

  return 0;
}

static int
ensure_docdir_inode_by_name (XdpInode                *parent,
                             int                      dirfd,
                             const char              *name,
                             struct fuse_entry_param *e)
{
  int o_path_fd;

  o_path_fd = openat (dirfd, name, O_PATH|O_NOFOLLOW, 0);
  if (o_path_fd == -1)
      return -errno;

  return ensure_docdir_inode (parent, o_path_fd, e, NULL); /* Takes ownership of o_path_fd */
}


static XdpInode *
ensure_by_app_inode (XdpInode   *by_app_inode,
                     const char *app_id)
{
  XdpDomain *by_app_domain = by_app_inode->domain;
  g_autoptr(XdpInode) inode = NULL;

  if (!xdp_is_valid_app_id (app_id))
    return NULL;

  G_LOCK (domain_inodes);
  inode = g_hash_table_lookup (by_app_domain->inodes, app_id);
  if (inode != NULL)
    inode = xdp_inode_ref (inode);
  else
    {
      g_autoptr(XdpDomain) app_domain = xdp_domain_new_app (by_app_inode, app_id);
      inode = xdp_inode_new (app_domain, NULL);
      g_hash_table_insert (by_app_domain->inodes, app_domain->app_id, inode);
    }
  G_UNLOCK (domain_inodes);

  return g_steal_pointer (&inode);
}

static XdpInode *
ensure_doc_inode (XdpInode   *parent,
                  const char *doc_id)
{
  g_autoptr(PermissionDbEntry) doc_entry = NULL;
  g_autoptr(XdpInode) inode = NULL;
  XdpDomain *parent_domain = parent->domain;

  doc_entry = xdp_lookup_doc (doc_id);

  if (doc_entry == NULL ||
      (parent_domain->app_id &&
       !app_can_see_doc (doc_entry, parent_domain->app_id)))
    return NULL;

  G_LOCK (domain_inodes);
  inode = g_hash_table_lookup (parent_domain->inodes, doc_id);
  if (inode != NULL)
    inode = xdp_inode_ref (inode);
  else
    {
      g_autoptr(XdpDomain) doc_domain = xdp_domain_new_document (parent_domain, doc_id, doc_entry);
      doc_domain->parent_inode = xdp_inode_ref (parent);
      inode = xdp_inode_new (doc_domain, NULL);
      g_hash_table_insert (parent_domain->inodes, doc_domain->doc_id, inode);
    }
  G_UNLOCK (domain_inodes);

  return g_steal_pointer (&inode);
}

static gboolean
invalidate_dentry_cb (gpointer user_data)
{
  GList *to_invalidate = NULL;
  {
    XDP_AUTOLOCK (invalidate_list);
    to_invalidate = g_steal_pointer (&invalidate_list);
  }
  to_invalidate = g_list_reverse (to_invalidate);

  XDP_AUTOLOCK (session);
  for (GList *l = to_invalidate; l != NULL; l = l->next)
    {
      XdpInvalidateData *data = l->data;
      if (session)
        fuse_lowlevel_notify_inval_entry (session, data->parent_ino, data->name,
                                          strlen (data->name));
      g_free (data);
    }

  g_list_free (to_invalidate);

  return FALSE;
}

/* Queue an inval_dentry, thereby freeing unused inodes in the dcache
 * which will free up a bunch of O_PATH fds in the fuse implementation.
 */
static void
queue_invalidate_dentry (XdpInode   *parent,
                         const char *name)
{
  XDP_AUTOLOCK (invalidate_list);

  for (GList *l = invalidate_list; l != NULL; l = l->next)
    {
      XdpInvalidateData *data = l->data;
      if (data->parent_ino == parent->ino && strcmp (name, data->name) == 0)
        return;
    }

  XdpInvalidateData *data = g_malloc0 (sizeof (XdpInvalidateData) + strlen (name) + 1);
  data->parent_ino = parent->ino;
  strcpy (data->name, name);

  if (invalidate_list == NULL)
    g_timeout_add (10, invalidate_dentry_cb, NULL);

  invalidate_list = g_list_append (invalidate_list, data);
}

static void
xdp_fuse_lookup (fuse_req_t  req,
                 fuse_ino_t  parent_ino,
                 const char *name)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  XdpDomain *parent_domain = parent->domain;
  g_autoptr(XdpInode) inode = NULL;
  struct fuse_entry_param e;
  int res, fd;
  int open_flags = O_PATH|O_NOFOLLOW;
  const char *op = "LOOKUP";

  g_debug ("LOOKUP %" G_GINT64_MODIFIER "x:%s", parent_ino, name);

  if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
    {
      /* We don't set FUSE_CAP_EXPORT_SUPPORT, so should not get
       * here. But lets make sure we never ever resolve them as that
       * could be a security issue by escaping the root. */
      return xdp_reply_err (op, req, ESTALE);
    }

  if (xdp_domain_is_virtual_type (parent_domain))
    {
      switch (parent_domain->type)
        {
        case XDP_DOMAIN_ROOT:
          if (strcmp (name, BY_APP_NAME) == 0)
            inode = xdp_inode_ref (by_app_inode);
          else
            inode = ensure_doc_inode (parent, name);
          break;
        case XDP_DOMAIN_BY_APP:
          inode = ensure_by_app_inode (parent, name);
          break;
        case XDP_DOMAIN_APP:
          inode = ensure_doc_inode (parent, name);
          break;
        default:
          g_assert_not_reached ();
        }

      if (inode == NULL)
        return xdp_reply_err (op, req, ENOENT);

      prepare_reply_virtual_entry (inode, &e);
    }
  else
    {
      g_assert (parent_domain->type == XDP_DOMAIN_DOCUMENT);

      fd = xdp_document_inode_open_child_fd (parent, name, open_flags, 0);
      if (fd < 0)
        return xdp_reply_err (op, req, -fd);

      res = ensure_docdir_inode (parent, fd, &e, NULL); /* Takes ownership of fd */
      if (res != 0)
        return xdp_reply_err (op, req, -res);

      queue_invalidate_dentry (parent, name);
    }

  g_debug ("LOOKUP %" G_GINT64_MODIFIER "x:%s => %" G_GINT64_MODIFIER "x", parent_ino, name, e.ino);

  if (fuse_reply_entry (req, &e) == -ENOENT)
    abort_reply_entry (&e);
}

static XdpFile *
xdp_file_new (int fd)
{
  XdpFile *file = g_new0 (XdpFile, 1);
  file->fd = fd;

  XDP_AUTOLOCK (open_files);
  open_files = g_list_prepend (open_files, file);
  file->link = open_files;

  return file;
}

static void
xdp_file_free (XdpFile *file)
{
  GList *link = g_steal_pointer (&file->link);

  XDP_AUTOLOCK (open_files);
  open_files = g_list_delete_link (open_files, link);

  close (file->fd);
  g_free (file);
}

static void
xdp_fuse_open (fuse_req_t             req,
               fuse_ino_t             ino,
               struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  int open_flags = fi->flags;
  g_autofree char *open_flags_string = open_flags_to_string (open_flags);
  int fd;
  g_autofree char *path = NULL;
  XdpFile *file = NULL;
  XdpDocumentChecks checks;
  const char *op = "OPEN";

  g_debug ("OPEN %" G_GINT64_MODIFIER "x %s", ino, open_flags_string);

  checks = CHECK_IS_PHYSICAL;
  if (open_flags_has_write (open_flags))
    checks |= CHECK_CAN_WRITE;

  /* Note: open_flags guaranteed to exclude O_CREAT, O_EXCL */
  if (!xdp_document_inode_checks (op, req, inode, checks))
    return;

  path = fd_to_path (inode->physical->fd);

  /*
   * `path` is a path to the fd entry in `/proc`, which is a symlink
   * to the actual file. Opening it directly with `O_NOFOLLOW` will
   * fail. So we should resolve it first then we can honour the no
   * follow flag.
   */
  if (open_flags & O_NOFOLLOW)
    {
      char resolved_path[PATH_MAX] = { 0, };
      ssize_t res;

      res = readlink (path, resolved_path, sizeof (resolved_path));

      if (res == sizeof (resolved_path))
        return xdp_reply_err (op, req, ENAMETOOLONG);
      if (res < 0)
        return xdp_reply_err (op, req, errno);

      g_clear_pointer (&path, g_free);
      path = g_strdup (resolved_path);
    }

  fd = open (path, open_flags, 0);
  if (fd == -1)
    return xdp_reply_err (op, req, errno);

  file = xdp_file_new (fd);

  fi->fh = (gsize)file;
  if (fuse_reply_open (req, fi) == -ENOENT)
    {
      /* The open syscall was interrupted, so it must be cancelled */
      xdp_file_free (file);
    }
}

static void
xdp_fuse_create (fuse_req_t             req,
                 fuse_ino_t             parent_ino,
                 const char            *filename,
                 mode_t                 mode,
                 struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  int open_flags = fi->flags;
  g_autofree char *open_flags_string = open_flags_to_string (open_flags);
  struct fuse_entry_param e;
  int res;
  g_autofd int fd = -1;
  g_autofd int o_path_fd = -1;
  g_autofree char *fd_path = NULL;
  XdpFile *file = NULL;
  const char *op = "CREATE";

  g_debug ("CREATE %" G_GINT64_MODIFIER "x %s %s, 0%o", parent_ino, filename, open_flags_string, mode);

  if (!xdp_document_inode_checks (op, req, parent,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_PHYSICAL_IF_DIR))
    return;

  fd = xdp_document_inode_open_child_fd (parent, filename, open_flags, mode);
  if (fd < 0)
    return xdp_reply_err (op, req, -fd);

  fd_path = fd_to_path (fd);
  o_path_fd = open (fd_path, O_PATH, 0);
  if (o_path_fd < 0)
    return xdp_reply_err (op, req, errno);

  res = ensure_docdir_inode (parent, g_steal_fd (&o_path_fd), &e, NULL); /* Takes ownership of o_path_fd */
  if (res != 0)
    return xdp_reply_err (op, req, -res);

  file = xdp_file_new (g_steal_fd (&fd)); /* Takes ownership of fd */

  fi->fh = (gsize)file;
  if (fuse_reply_create (req, &e, fi) == -ENOENT)
    {
      /* The open syscall was interrupted, so it must be cancelled */
      xdp_file_free (file);
      abort_reply_entry (&e);
    }

  queue_invalidate_dentry (parent, filename);
}

static void
xdp_fuse_read (fuse_req_t             req,
               fuse_ino_t             ino,
               size_t                 size,
               off_t                  off,
               struct fuse_file_info *fi)
{
  struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
  XdpFile *file = (XdpFile *)fi->fh;
  enum fuse_buf_copy_flags reply_flags = FUSE_BUF_SPLICE_MOVE;
  XdpFuseOptions *fuse_opts = fuse_req_userdata (req);

  g_debug ("READ %" G_GINT64_MODIFIER "x size %" G_GSIZE_FORMAT " off %" G_GOFFSET_FORMAT, ino, size, (goffset)off);

  buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  buf.buf[0].fd = file->fd;
  buf.buf[0].pos = off;

  if (!fuse_opts->use_splice)
    reply_flags = FUSE_BUF_NO_SPLICE;

  fuse_reply_data (req, &buf, reply_flags);
}


static void
xdp_fuse_write (fuse_req_t             req,
                fuse_ino_t             ino,
                const char            *buf,
                size_t                 size,
                off_t                  off,
                struct fuse_file_info *fi)
{
  XdpFile *file = (XdpFile *)fi->fh;
  ssize_t res;
  const char *op = "WRITE";

  g_debug ("WRITE %" G_GINT64_MODIFIER "x size %" G_GSIZE_FORMAT " off %" G_GOFFSET_FORMAT, ino, size, (goffset)off);

  res = pwrite (file->fd, buf, size, off);

  if (res >= 0)
    fuse_reply_write (req, res);
  else
    xdp_reply_err (op, req, errno);
}

static void
xdp_fuse_write_buf (fuse_req_t             req,
                    fuse_ino_t             ino,
                    struct fuse_bufvec    *bufv,
                    off_t                  off,
                    struct fuse_file_info *fi)
{
  XdpFile *file = (XdpFile *)fi->fh;
  struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(bufv));
  ssize_t res;
  const char *op = "WRITEBUF";
  enum fuse_buf_copy_flags copy_flags = FUSE_BUF_SPLICE_NONBLOCK;
  XdpFuseOptions *fuse_opts = fuse_req_userdata (req);

  g_debug ("WRITEBUF %" G_GINT64_MODIFIER "x off %" G_GOFFSET_FORMAT, ino, (goffset)off);

  dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  dst.buf[0].fd = file->fd;
  dst.buf[0].pos = off;

  if (!fuse_opts->use_splice)
    copy_flags = FUSE_BUF_NO_SPLICE;

  res = fuse_buf_copy (&dst, bufv, copy_flags);
  if (res >= 0)
    fuse_reply_write (req, res);
  else
    xdp_reply_err (op, req, errno);
}

static void
xdp_fuse_fsync (fuse_req_t             req,
                fuse_ino_t             ino,
                int                    datasync,
                struct fuse_file_info *fi)
{
  XdpFile *file = (XdpFile *)fi->fh;
  int res;
  const char *op = "FSYNC";

  g_debug ("FSYNC %" G_GINT64_MODIFIER "x", ino);

  if (datasync)
    res = fdatasync (file->fd);
  else
    res = fsync (file->fd);
  if (res == 0)
    xdp_reply_ok (op, req);
  else
    xdp_reply_err (op, req, errno);
}

static void
xdp_fuse_fallocate (fuse_req_t             req,
                    fuse_ino_t             ino,
                    int                    mode,
                    off_t                  offset,
                    off_t                  length,
                    struct fuse_file_info *fi)
{
  XdpFile *file = (XdpFile *)fi->fh;
  int res;
  const char *op = "FALLOCATE";

  g_debug ("FALLOCATE %" G_GINT64_MODIFIER "x", ino);

#ifdef __linux__
  res = fallocate (file->fd, mode, offset, length);
#else
  res = posix_fallocate (file->fd, offset, length);
#endif

  if (res == 0)
    xdp_reply_ok (op, req);
  else
    xdp_reply_err (op, req, errno);
}

static void
xdp_fuse_flush (fuse_req_t             req,
                fuse_ino_t             ino,
                struct fuse_file_info *fi)
{
  const char *op = "FLUSH";

  g_debug ("FLUSH %" G_GINT64_MODIFIER "x", ino);
  xdp_reply_ok (op, req);
}

static void
xdp_fuse_release (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct fuse_file_info *fi)
{
  XdpFile *file = (XdpFile *)fi->fh;
  const char *op = "RELEASE";

  g_debug ("RELEASE %" G_GINT64_MODIFIER "x", ino);

  xdp_file_free (file);

  xdp_reply_ok (op, req);
}

static void
forget_one (fuse_ino_t ino,
            unsigned long nlookup)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);

  g_debug ("FORGET %" G_GINT64_MODIFIER "x %ld", ino, nlookup);
  xdp_inode_kernel_unref (inode, nlookup);
}

static void
xdp_fuse_forget (fuse_req_t req,
                 fuse_ino_t ino,
                 uint64_t   nlookup)
{
  forget_one (ino, nlookup);
  fuse_reply_none (req);
}

static void
xdp_fuse_forget_multi (fuse_req_t               req,
                       size_t                   count,
                       struct fuse_forget_data *forgets)
{
  size_t i;

  g_debug ("FORGET_MULTI %" G_GSIZE_FORMAT, count);

  for (i = 0; i < count; i++)
    forget_one (forgets[i].ino, forgets[i].nlookup);

  fuse_reply_none (req);
}

static void
xdp_dir_free (XdpDir *d)
{
  if (d->dir)
    closedir (d->dir);
  g_free (d->dirbuf);
  g_free (d);
}

static void
xdp_dir_add (XdpDir     *d,
             fuse_req_t  req,
             const char *name,
             mode_t      mode)
{
  struct stat stbuf;

  size_t oldsize = d->dirbuf_size;

  d->dirbuf_size += fuse_add_direntry (req, NULL, 0, name, NULL, 0);
  d->dirbuf = (char *) g_realloc (d->dirbuf, d->dirbuf_size);
  memset (&stbuf, 0, sizeof (stbuf));
  stbuf.st_ino = FUSE_UNKNOWN_INO;
  stbuf.st_mode = mode;
  fuse_add_direntry (req, d->dirbuf + oldsize,
                     d->dirbuf_size - oldsize,
                     name, &stbuf,
                     d->dirbuf_size);
}

static XdpDir *
xdp_dir_new_physical (DIR *dir)
{
  XdpDir *d = g_new0 (XdpDir, 1);
  d->dir = dir;
  d->offset = 0;
  d->entry = NULL;
  return d;
}

static XdpDir *
xdp_dir_new_buffered (fuse_req_t  req)
{
  XdpDir *d = g_new0 (XdpDir, 1);
  xdp_dir_add (d, req, ".", S_IFDIR);
  xdp_dir_add (d, req, "..", S_IFDIR);
  return d;
}

static void
xdp_dir_add_docs (XdpDir     *d,
                  fuse_req_t  req,
                  const char *for_app_id)
{
  g_auto(GStrv) docs = NULL;
  int i;

  docs = xdp_list_docs ();
  for (i = 0; docs[i] != NULL; i++)
    {
      if (for_app_id)
        {
          g_autoptr(PermissionDbEntry) entry = xdp_lookup_doc (docs[i]);
          if (entry == NULL ||
              !app_can_see_doc (entry, for_app_id))
            continue;
        }

      xdp_dir_add (d, req, docs[i], S_IFDIR);
    }
}

static void
xdp_dir_add_apps (XdpDir     *d,
                  XdpDomain  *domain,
                  fuse_req_t  req,
                  const char *for_app_id)
{
  g_auto(GStrv) apps = NULL;
  g_auto(GStrv) names = NULL;
  int i;

  /* First all pre-used apps as these can be created on demand */
  names = xdp_domain_get_inode_keys_as_string (domain);
  for (i = 0; names[i] != NULL; i++)
    xdp_dir_add (d, req, names[i], S_IFDIR);

  /* Then all in the db (that don't already have inodes) */
  apps = xdp_list_apps ();
  for (i = 0; apps[i] != NULL; i++)
    {
      const char *app = apps[i];
      if (!g_strv_contains ((const gchar * const *)names, app))
        xdp_dir_add (d, req, app, S_IFDIR);
    }
}

static void
xdp_fuse_opendir (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  XdpDomain *domain = inode->domain;
  XdpDir *d = NULL;
  const char *op = "OPENDIR";

  g_debug ("OPENDIR %" G_GINT64_MODIFIER "x domain %d", ino, inode->domain->type);

  if (xdp_domain_is_virtual_type (domain))
    {
      d = xdp_dir_new_buffered (req);
      switch (domain->type)
        {
        case XDP_DOMAIN_ROOT:
          xdp_dir_add (d, req, BY_APP_NAME, S_IFDIR);
          xdp_dir_add_docs (d, req, NULL);
          break;
        case XDP_DOMAIN_APP:
          xdp_dir_add_docs (d, req, domain->app_id);
          break;
        case XDP_DOMAIN_BY_APP:
          xdp_dir_add_apps (d, inode->domain, req, NULL);
          break;
        default:
          g_assert_not_reached ();
        }
    }
  else
    {
      g_assert (domain->type == XDP_DOMAIN_DOCUMENT);

      if (xdp_document_domain_is_dir (domain))
        {
          if (inode->physical)
            {
              DIR *dir;
              int fd;

              fd = openat (inode->physical->fd, ".", O_RDONLY | O_DIRECTORY, 0);
              if (fd < 0)
                return xdp_reply_err (op, req, errno);

              dir = fdopendir (fd);
              if (dir == NULL)
                {
                  xdp_reply_err (op, req, errno);
                  close (fd);
                  return;
                }

              d = xdp_dir_new_physical (dir);
            }
          else /* Nonphysical, i.e. toplevel */
            {
              struct stat buf;

              d = xdp_dir_new_buffered (req);

              if (stat (domain->doc_path, &buf) == 0 &&
                  buf.st_ino == domain->doc_dir_inode &&
                  buf.st_dev == domain->doc_dir_device)
                {
                  xdp_dir_add (d, req, domain->doc_file, buf.st_mode);
                }
            }
        }
      else
        {
          g_autofree char *main_path = g_build_filename (domain->doc_path, domain->doc_file, NULL);
          struct stat buf;
          GHashTableIter iter;
          gpointer key, value;

          d = xdp_dir_new_buffered (req);

          if (stat (main_path, &buf) == 0)
            xdp_dir_add (d, req, domain->doc_file, buf.st_mode);

          g_mutex_lock (&domain->tempfile_mutex);

          g_hash_table_iter_init (&iter, domain->tempfiles);
          while (g_hash_table_iter_next (&iter, &key, &value))
            {
              const char *tempname = key;
              xdp_dir_add (d, req, tempname, S_IFREG);
            }

          g_mutex_unlock (&domain->tempfile_mutex);
        }
    }

  fi->fh = (gsize)d;

  if (fuse_reply_open (req, fi) == -ENOENT)
    {
      /* The opendir syscall was interrupted, so it must be cancelled */
      xdp_dir_free (d);
    }
}

static void
xdp_fuse_readdir (fuse_req_t             req,
                  fuse_ino_t             ino,
                  size_t                 size,
                  off_t                  off,
                  struct fuse_file_info *fi)
{
  XdpDir *d = (XdpDir *)fi->fh;
  const char *op = "READDIR";

  g_debug ("READDIR %" G_GINT64_MODIFIER "x %" G_GSIZE_FORMAT " %" G_GOFFSET_FORMAT, ino, size, (goffset)off);

  if (d->dir)
    {
      g_autofree char *buf = g_try_malloc (size);
      size_t rem;
      char *p;

      if (buf == NULL)
        {
          xdp_reply_err (op, req, ENOMEM);
          return;
        }

      /* If offset is not same, need to seek it */
      if (off != d->offset)
        {
          seekdir (d->dir, off);
          d->entry = NULL;
          d->offset = off;
        }

      p = buf;
      rem = size;
      while (TRUE)
        {
          size_t entsize;
          off_t nextoff;

          if (!d->entry)
            {
              errno = 0;
              d->entry = readdir (d->dir);
              if (!d->entry)
                {
                  if (errno && rem == size)
                    {
                      xdp_reply_err (op, req, errno);
                      return;
                    }
                  break;
                }
            }
          nextoff = telldir (d->dir);

          struct stat st = {
            .st_ino = FUSE_UNKNOWN_INO,
            .st_mode = d->entry->d_type << 12,
          };
          entsize = fuse_add_direntry (req, p, rem,
                                       d->entry->d_name, &st, nextoff);
          /* The above function returns the size of the entry size even though
           * the copy failed due to smaller buf size, so I'm checking after this
           * function and breaking out in case we exceed the size.
           */
          if (entsize > rem)
            break;

          p += entsize;
          rem -= entsize;

          d->entry = NULL;
          d->offset = nextoff;
        }

      fuse_reply_buf(req, buf, size - rem);
    }
  else
    {
      if (off < d->dirbuf_size)
        {
          gsize reply_size = MIN (d->dirbuf_size - off, size);
          g_autofree char *buf = g_memdup2 (d->dirbuf + off, reply_size);
          fuse_reply_buf (req, buf, reply_size);
        }
      else
        {
          fuse_reply_buf (req, NULL, 0);
        }
    }
}

static void
xdp_fuse_releasedir (fuse_req_t             req,
                     fuse_ino_t             ino,
                     struct fuse_file_info *fi)
{
  XdpDir *d = (XdpDir *)fi->fh;
  const char *op = "RELEASEDIR";

  g_debug ("RELEASEDIR %" G_GINT64_MODIFIER "x", ino);

  xdp_dir_free (d);

  xdp_reply_ok (op, req);
}

static void
xdp_fuse_fsyncdir (fuse_req_t             req,
                   fuse_ino_t             ino,
                   int                    datasync,
                   struct fuse_file_info *fi)
{
  XdpDir *dir = (XdpDir *)fi->fh;
  int fd, res;
  const char *op = "FSYNCDIR";

  g_debug ("FSYNCDIR %" G_GINT64_MODIFIER "x", ino);

  if (dir->dir)
    {
      fd = dirfd (dir->dir);
      if (datasync)
        res = fdatasync (fd);
      else
        res = fsync (fd);
    }
  else
    res = 0;

  if (res == 0)
    xdp_reply_ok (op, req);
  else
    xdp_reply_err (op, req, errno);
}

static void
xdp_fuse_mkdir (fuse_req_t  req,
                fuse_ino_t  parent_ino,
                const char *name,
                mode_t      mode)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  struct fuse_entry_param e;
  int res;
  g_autofd int close_fd = -1;
  int dirfd;
  const char *op = "MKDIR";

  g_debug ("MKDIR %" G_GINT64_MODIFIER "x %s", parent_ino, name);

  if (!xdp_document_inode_checks (op, req, parent,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  dirfd = xdp_document_inode_ensure_dirfd (parent, &close_fd);
  if (dirfd < 0)
    return xdp_reply_err (op, req, -dirfd);

  res = mkdirat (dirfd, name, mode);
  if (res != 0)
    return xdp_reply_err (op, req, errno);

  res = ensure_docdir_inode_by_name (parent, dirfd, name, &e); /* Takes ownership of o_path_fd */
  if (res != 0)
    return xdp_reply_err (op, req, -res);

  if (fuse_reply_entry (req, &e) == -ENOENT)
    abort_reply_entry (&e);
}

static void
xdp_fuse_unlink (fuse_req_t  req,
                 fuse_ino_t  parent_ino,
                 const char *filename)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  XdpDomain *parent_domain = parent->domain;
  int res = -1;
  const char * op = "UNLINK";

  g_debug ("UNLINK %" G_GINT64_MODIFIER "x %s", parent_ino, filename);

  if (!xdp_document_inode_checks (op, req, parent,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_PHYSICAL_IF_DIR))
    return;

  if (parent->physical)
    {
      res = unlinkat (parent->physical->fd, filename, 0);
      if (res != 0)
        return xdp_reply_err (op, req, errno);
    }
  else
    {
      g_autofd int dirfd = -1;

      /* Only reached for non-directory inodes */

      dirfd = xdp_nonphysical_document_inode_opendir (parent);
      if (dirfd < 0)
        xdp_reply_err (op, req, -dirfd);

      if (strcmp (filename, parent_domain->doc_file) == 0)
        {
          res = unlinkat (dirfd, filename, 0);
          if (res != 0)
            return xdp_reply_err (op, req, errno);
        }
      else
        {
          gboolean removed = FALSE;

          /* Not directory and not main file, maybe a temporary file? */
          g_mutex_lock (&parent_domain->tempfile_mutex);
          removed = g_hash_table_remove (parent_domain->tempfiles, filename);
          g_mutex_unlock (&parent_domain->tempfile_mutex);

          if (!removed)
            return xdp_reply_err (op, req, ENOENT);
        }
    }

  xdp_reply_ok (op, req);
}

static int
try_renameat (int           olddirfd,
              const char   *oldpath,
              int           newdirfd,
              const char   *newpath,
              unsigned int  flags)
{
#if HAVE_RENAMEAT2
  return renameat2 (olddirfd, oldpath, newdirfd, newpath, flags);
#else
  if (flags)
    {
      g_warning ("renameat2 is not supported by this system and rename flags are set");
      errno = EINVAL;
      return -1;
    }

  return renameat (olddirfd, oldpath, newdirfd, newpath);
#endif
}


static void
xdp_fuse_rename (fuse_req_t    req,
                 fuse_ino_t    parent_ino,
                 const char   *name,
                 fuse_ino_t    newparent_ino,
                 const char   *newname,
                 unsigned int  flags)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  g_autoptr(XdpInode) newparent = xdp_inode_from_ino (newparent_ino);
  g_autofree char *rename_flags_string = renameat2_flags_to_string (flags);
  XdpDomain *domain;
  int res, errsv;
  int olddirfd, newdirfd, dirfd;
  g_autofd int close_fd1 = -1;
  g_autofd int close_fd2 = -1;
  const char *op = "RENAME";

  g_debug ("RENAME %" G_GINT64_MODIFIER "x %s -> %" G_GINT64_MODIFIER "x %s (flags: %s)", parent_ino, name,
           newparent_ino, newname, rename_flags_string);

  if (!xdp_document_inode_checks (op, req, parent,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_PHYSICAL_IF_DIR))
    return;

  /* Don't allow cross-domain renames */
  if (parent->domain != newparent->domain)
    return xdp_reply_err (op, req, EXDEV);

  domain = parent->domain;
  if (xdp_document_domain_is_dir (domain))
    {
      olddirfd = xdp_document_inode_ensure_dirfd (parent, &close_fd1);
      if (olddirfd < 0)
        return xdp_reply_err (op, req, -olddirfd);

      newdirfd = xdp_document_inode_ensure_dirfd (newparent, &close_fd2);
      if (newdirfd < 0)
        return xdp_reply_err (op, req, -newdirfd);

      res = try_renameat (olddirfd, name, newdirfd, newname, flags);
      if (res != 0)
        return xdp_reply_err (op, req, errno);

      xdp_reply_ok (op, req);
    }
  else
    {
      /* For non-directories, only allow renames in toplevel (nonphysical) dir */
      if (parent != newparent || parent->physical != NULL)
        return xdp_reply_err (op, req, EACCES);

      /* Early exit for same file */
      if (strcmp (name, newname) == 0)
        return xdp_reply_ok (op, req);

      dirfd = xdp_nonphysical_document_inode_opendir (parent);
      if (dirfd < 0)
        return xdp_reply_err (op, req, -dirfd);
      close_fd1 = dirfd;

      if (strcmp (name, domain->doc_file) == 0)
        {
          /* Source is (maybe) main file, destination is tempfile */
          g_autofree char *tmpname = NULL;
          int tmp_fd;

          /* Just use this to get an exclusive name, we will later replace its content */
          tmp_fd = open_temp_at (dirfd, newname, &tmpname, 0600);
          if (tmp_fd < 0)
            return xdp_reply_err (op, req, -tmp_fd);
          close (tmp_fd);

          g_mutex_lock (&domain->tempfile_mutex);
          res = try_renameat (dirfd, name, dirfd, tmpname, flags);
          if (res == -1)
            {
              res = -errno;
              /* Remove the temporary file if the move failed */
              (void) unlinkat (dirfd, tmpname, 0);
            }
          else
            {
              res = get_tempfile_for (parent, domain, newname, dirfd, tmpname, NULL);
            }

          g_mutex_unlock (&domain->tempfile_mutex);

          if (res != 0)
            return xdp_reply_err (op, req, -res);

          xdp_reply_ok (op, req);
        }
      else if (strcmp (newname, domain->doc_file) == 0)
        {
          gpointer stolen_value;

          /* source is (maybe) tempfile, Destination is main file */

          g_mutex_lock (&domain->tempfile_mutex);
          if (g_hash_table_steal_extended (domain->tempfiles, name,
                                           NULL, &stolen_value))
            {
              XdpTempfile *tempfile = stolen_value;

              res = try_renameat (dirfd, tempfile->tempname, dirfd, newname, flags);
              errsv = errno;

              if (res == -1) /* Revert tempfile steal */
                g_hash_table_replace (domain->tempfiles, tempfile->name, tempfile);
              else
                {
                  /* Steal the old tempname so we don't unlink it */
                  g_free (g_steal_pointer (&tempfile->tempname));
                  xdp_tempfile_unref (tempfile);
                }
            }
          else
            {
              res = -1;
              errsv = ENOENT;
            }

          g_mutex_unlock (&domain->tempfile_mutex);

          if (res != 0)
            return xdp_reply_err (op, req, errsv);

          xdp_reply_ok (op, req);
        }
      else
        {
          /* Source and destinations are both tempfiles, no need to change anything on disk */
          gboolean found_tempfile = FALSE;
          gpointer stolen_value;

          /* Renaming temp file to temp file */
          g_mutex_lock (&domain->tempfile_mutex);
          if (g_hash_table_steal_extended (domain->tempfiles, name,
                                            NULL, &stolen_value))
            {
              XdpTempfile *tempfile = stolen_value;

              found_tempfile = TRUE;

              g_free (tempfile->name);
              tempfile->name = g_strdup (newname);

              /* This destroys any pre-existing tempfile with this name */
              g_hash_table_replace (domain->tempfiles, tempfile->name, tempfile);
          }
          g_mutex_unlock (&domain->tempfile_mutex);

          if (!found_tempfile)
            return xdp_reply_err (op, req, ENOENT);

          xdp_reply_ok (op, req);
        }
    }
}

static void
xdp_fuse_access (fuse_req_t req,
                 fuse_ino_t ino,
                 int        mask)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  g_autofree char *path = NULL;
  int res;
  const char *op = "ACCESS";

  g_debug ("ACCESS %" G_GINT64_MODIFIER "x", ino);

  if (inode->domain->type != XDP_DOMAIN_DOCUMENT)
    {
      if (mask & W_OK)
        return xdp_reply_err (op, req, EPERM);
      else
        return xdp_reply_ok (op, req);
    }

  if ((mask & W_OK) != 0 &&
      !xdp_document_domain_can_write (inode->domain))
    return xdp_reply_err (op, req, EPERM);

  if (inode->physical)
    {
      path = fd_to_path (inode->physical->fd);
      res = access (path, mask);
    }
  else
    {
      if (xdp_document_domain_is_dir (inode->domain))
        {
          if (mask & W_OK)
            res = EPERM;
          else
            res = 0;
        }
      else
        res = access (inode->domain->doc_path, mask);
    }

  if (res == -1)
    xdp_reply_err (op, req, errno);
  else
    xdp_reply_ok (op, req);
}

static void
xdp_fuse_rmdir (fuse_req_t  req,
                fuse_ino_t  parent_ino,
                const char *filename)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  g_autofd int close_fd = -1;
  int dirfd;
  int res;
  const char *op = "RMDIR";

  g_debug ("RMDIR %" G_GINT64_MODIFIER "x %s", parent_ino, filename);

  if (!xdp_document_inode_checks (op, req, parent,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  dirfd = xdp_document_inode_ensure_dirfd (parent, &close_fd);
  if (dirfd < 0)
    return xdp_reply_err (op, req, -dirfd);

  res = unlinkat (dirfd, filename, AT_REMOVEDIR);
  if (res != 0)
    return xdp_reply_err (op, req, errno);

  xdp_reply_ok (op, req);
}

static void
xdp_fuse_readlink (fuse_req_t req,
                   fuse_ino_t ino)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  char linkname[PATH_MAX + 1];
  const char *op = "READLINK";
  ssize_t res;

  g_debug ("READLINK %" G_GINT64_MODIFIER "x", ino);

  if (!xdp_document_inode_checks (op, req, inode,
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  if (inode->physical == NULL)
    return xdp_reply_err (op, req, EINVAL);

  res = readlinkat (inode->physical->fd, "", linkname, sizeof(linkname));
  if (res < 0)
    return xdp_reply_err (op, req, errno);

  linkname[res] = '\0';
  fuse_reply_readlink (req, linkname);
}

static void
xdp_fuse_symlink (fuse_req_t  req,
                  const char *link,
                  fuse_ino_t  parent_ino,
                  const char *name)
{
  g_autoptr(XdpInode) parent = xdp_inode_from_ino (parent_ino);
  g_autofd int close_fd = -1;
  struct fuse_entry_param e;
  const char * op = "SYMLINK";
  int dirfd;
  int res;

  g_debug ("SYMLINK %s %" G_GINT64_MODIFIER "x %s", link, parent_ino, name);

  if (!xdp_document_inode_checks (op, req, parent,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  dirfd = xdp_document_inode_ensure_dirfd (parent, &close_fd);
  if (dirfd < 0)
    return xdp_reply_err (op, req, -dirfd);

  res = symlinkat (link, dirfd, name);
  if (res != 0)
    return xdp_reply_err (op, req, errno);

  res = ensure_docdir_inode_by_name (parent, dirfd, name, &e); /* Takes ownership of o_path_fd */
  if (res != 0)
    return xdp_reply_err (op, req, -res);

  if (fuse_reply_entry (req, &e) == -ENOENT)
    abort_reply_entry (&e);
}

static void
xdp_fuse_link (fuse_req_t  req,
               fuse_ino_t  ino,
               fuse_ino_t  newparent_ino,
               const char *newname)
{
  g_autoptr(XdpInode) newparent = xdp_inode_from_ino (newparent_ino);
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  g_autofree char *proc_path = NULL;
  g_autofd int close_fd = -1;
  struct fuse_entry_param e;
  const char * op = "LINK";
  int newparent_dirfd;
  int res;

  g_debug ("LINK %" G_GINT64_MODIFIER "x %" G_GINT64_MODIFIER "x %s", ino, newparent_ino, newname);

  /* hardlinks only supported in docdirs, and only physical files */
  if (!xdp_document_inode_checks (op, req, inode,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  /* Don't allow linking between domains */
  if (inode->domain != newparent->domain)
    return xdp_reply_err (op, req, EXDEV);

  proc_path = fd_to_path (inode->physical->fd);
  newparent_dirfd = xdp_document_inode_ensure_dirfd (newparent, &close_fd);
  if (newparent_dirfd < 0)
    return xdp_reply_err (op, req, -newparent_dirfd);

  res = linkat (AT_FDCWD, proc_path, newparent_dirfd, newname, AT_SYMLINK_FOLLOW);
  if (res != 0)
    return xdp_reply_err (op, req, errno);

  res = ensure_docdir_inode_by_name (inode, newparent_dirfd, newname, &e); /* Takes ownership of o_path_fd */
  if (res != 0)
    return xdp_reply_err (op, req, -res);

  if (fuse_reply_entry (req, &e) == -ENOENT)
    abort_reply_entry (&e);
}


static void
xdp_fuse_statfs (fuse_req_t req,
                 fuse_ino_t ino)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  struct statvfs buf;
  int res;
  const char *op = "STATFS";

  g_debug ("STATFS %" G_GINT64_MODIFIER "x", ino);

  if(xdp_domain_is_virtual_type(inode->domain)) {
	memset(&buf, 0, sizeof(buf));
	fuse_reply_statfs (req, &buf);
	return;
  }

  if (!xdp_document_inode_checks (op, req, inode, 0))
    return;

  if (inode->physical)
    res = fstatvfs (inode->physical->fd, &buf);
  else
    res = statvfs (inode->domain->doc_path, &buf);

  if (!res)
    fuse_reply_statfs (req, &buf);
  else
    xdp_reply_err (op, req, errno);
}

static gboolean
xdp_fuse_get_real_path (XdpPhysicalInode  *physical,
                        char             **real_path_out)
{
  g_autofree char *fd_path = fd_to_path (physical->fd);
  char path_buffer[PATH_MAX + 1];
  DevIno file_devino = physical->backing_devino;
  ssize_t symlink_size;
  struct stat buf;

  /* Try to extract a real path to the file
   * (and verify it goes to the same place as the fd) */
  symlink_size = readlink (fd_path, path_buffer, PATH_MAX);
  if (symlink_size < 1)
    return FALSE;

  path_buffer[symlink_size] = 0;

  if (lstat (path_buffer, &buf) != 0 ||
      buf.st_dev != file_devino.dev ||
      buf.st_ino != file_devino.ino)
    return FALSE;

  *real_path_out = g_strdup (path_buffer);
  return TRUE;
}

static ssize_t
xdp_fuse_set_host_path_xattr (XdpInode   *inode,
                              const char *value,
                              size_t      size)
{
  errno = EPERM;
  return -1;
}

static ssize_t
xdp_fuse_get_host_path_xattr (XdpInode *inode,
                              char     *buf,
                              size_t    size)
{
  const char *path = NULL;
  size_t path_size;
  g_autofree char *real_path = NULL;

  path = inode->domain->doc_path;
  if (!path)
    {
      errno = ENODATA;
      return -1;
    }

  if (inode->physical)
    {
      if (!xdp_fuse_get_real_path (inode->physical, &real_path))
        {
          errno = ENODATA;
          return -1;
        }

      if (!g_str_has_prefix (real_path, path))
        {
          errno = ENODATA;
          return -1;
        }

      path = real_path;
    }

  path_size = strlen (path);

  if (size == 0)
    return path_size;

  if (size < path_size)
    {
      errno = ERANGE;
      return -1;
    }

  memcpy (buf, path, path_size);
  return path_size;
}

static ssize_t
xdp_fuse_remove_host_path_xattr (XdpInode *inode)
{
  errno = EPERM;
  return -1;
}

static void
xdp_fuse_setxattr (fuse_req_t  req,
                   fuse_ino_t  ino,
                   const char *name,
                   const char *value,
                   size_t      size,
                   int         flags)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  ssize_t res;
  g_autofree char *path = NULL;
  const char *op = "SETXATTR";

  g_debug ("SETXATTR %" G_GINT64_MODIFIER "x %s", ino, name);

  if (!xdp_document_inode_checks (op, req, inode,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  if (g_strcmp0 (name, XDP_XATTR_HOST_PATH) == 0)
    {
      res = xdp_fuse_set_host_path_xattr (inode, value, size);
    }
  else
    {
      path = fd_to_path (inode->physical->fd);
#if HAVE_SYS_XATTR_H
      res = setxattr (path, name, value, size, flags);
#elif HAVE_SYS_EXTATTR_H
      res = extattr_set_file (path, EXTATTR_NAMESPACE_USER, name, value, size);
#else
#error "Not implemented for your platform"
#endif
    }

  if (res < 0)
    return xdp_reply_err (op, req, errno);

  xdp_reply_ok (op, req);
}

static void
xdp_fuse_getxattr (fuse_req_t  req,
                   fuse_ino_t  ino,
                   const char *name,
                   size_t      size)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  ssize_t res;
  g_autofree char *buf = NULL;
  g_autofree char *path = NULL;
  const char *op = "GETXATTR";

  g_debug ("GETXATTR %" G_GINT64_MODIFIER "x %s %" G_GSIZE_FORMAT, ino, name, size);

  if (inode->domain->type != XDP_DOMAIN_DOCUMENT)
    return xdp_reply_err (op, req, ENODATA);

  if (size != 0)
    buf = g_malloc (size);

  path = xdp_document_inode_get_self_as_path (inode);
  if (path == NULL)
    return xdp_reply_err (op, req, ENODATA);

  if (g_strcmp0 (name, XDP_XATTR_HOST_PATH) == 0)
    {
      res = xdp_fuse_get_host_path_xattr (inode, buf, size);
    }
  else
    {
#if HAVE_SYS_XATTR_H
      res = getxattr (path, name, buf, size);
#elif HAVE_SYS_EXTATTR_H
      res = extattr_get_file (path, EXTATTR_NAMESPACE_USER, name, buf, size);
#else
#error "Not implemented for your platform"
#endif
    }

  if (res < 0)
    return xdp_reply_err (op, req, errno);

  if (size == 0)
    fuse_reply_xattr (req, res);
  else
    fuse_reply_buf (req, buf, res);
}

static ssize_t
xdp_fuse_listxattr_xdp_attrs (XdpInode *inode,
                              ssize_t   res,
                              char     *buf,
                              size_t    size)
{
  size_t attr_size = sizeof (XDP_XATTR_ATTRIBUTE_NAME_LIST);

  if (size == 0)
    return res + attr_size;

  if (size < res + attr_size)
    {
      errno = ERANGE;
      return -1;
    }

  memcpy (buf + res, XDP_XATTR_ATTRIBUTE_NAME_LIST, attr_size);
  return res + attr_size;
}

static void
xdp_fuse_listxattr (fuse_req_t req,
                    fuse_ino_t ino,
                    size_t     size)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  g_autofree char *path = NULL;
  g_autofree char *buf = NULL;
  const char *op = "LISTXATTR";
  ssize_t res;

  g_debug ("LISTXATTR %" G_GINT64_MODIFIER "x %" G_GSIZE_FORMAT, ino, size);

  if (inode->domain->type != XDP_DOMAIN_DOCUMENT)
    return xdp_reply_err (op, req, ENOTSUP);

  if (size != 0)
    buf = g_malloc (size);

  path = xdp_document_inode_get_self_as_path (inode);

  if (path == NULL)
    {
      res = 0;
    }
  else
    {
#if HAVE_SYS_XATTR_H
      res = listxattr (path, buf, size);
#elif HAVE_SYS_EXTATTR_H
      res = extattr_list_file (path, EXTATTR_NAMESPACE_USER, buf, size);
#else
#error "Not implemented for your platform"
#endif

      if (res >= 0)
        res = xdp_fuse_listxattr_xdp_attrs (inode, res, buf, size);
    }

  if (res < 0)
    return xdp_reply_err (op, req, errno);

  if (size == 0)
    fuse_reply_xattr (req, res);
  else
    fuse_reply_buf (req, buf, res);
}

static void
xdp_fuse_removexattr (fuse_req_t  req,
                      fuse_ino_t  ino,
                      const char *name)
{
  g_autoptr(XdpInode) inode = xdp_inode_from_ino (ino);
  g_autofree char *path = NULL;
  ssize_t res;
  const char *op = "REMOVEXATTR";

  g_debug ("REMOVEXATTR %" G_GINT64_MODIFIER "x %s", ino, name);

  if (!xdp_document_inode_checks (op, req, inode,
                                  CHECK_CAN_WRITE |
                                  CHECK_IS_DIRECTORY |
                                  CHECK_IS_PHYSICAL))
    return;

  if (g_strcmp0 (name, XDP_XATTR_HOST_PATH) == 0)
    {
      res = xdp_fuse_remove_host_path_xattr (inode);
    }
  else
    {
      path = fd_to_path (inode->physical->fd);
#if HAVE_SYS_XATTR_H
      res = removexattr (path, name);
#elif HAVE_SYS_EXTATTR_H
      res = extattr_delete_file (path, EXTATTR_NAMESPACE_USER, name);
#else
#error "Not implemented for your platform"
#endif
    }

  if (res < 0)
    xdp_reply_err (op, req, errno);
  else
    xdp_reply_ok (op, req);
}

static void
xdp_fuse_getlk (fuse_req_t             req,
                fuse_ino_t             ino,
                struct fuse_file_info *fi,
                struct flock          *lock)
{
  const char *op = "GETLK";
  XdpFile *file = (XdpFile *)fi->fh;
  int res;

  g_debug ("GETLK %" G_GINT64_MODIFIER "x", ino);

  res = fcntl (file->fd, F_GETLK, lock);
  if (res < 0)
    return xdp_reply_err (op, req, errno);

  return xdp_reply_ok (op, req);
}

static void
xdp_fuse_setlk (fuse_req_t             req,
                fuse_ino_t             ino,
                struct fuse_file_info *fi,
                struct flock          *lock,
                int                    sleep)
{
  const char *op = "SETLK";
  XdpFile *file = (XdpFile *)fi->fh;
  int res;

  g_debug ("SETLK %" G_GINT64_MODIFIER "x", ino);

  res = fcntl (file->fd, F_SETLK, lock);
  if (res < 0)
    return xdp_reply_err (op, req, errno);

  return xdp_reply_ok (op, req);
}

static void
xdp_fuse_flock (fuse_req_t             req,
                fuse_ino_t             ino,
                struct fuse_file_info *fi,
                int                    lock_op)
{
  const char *op = "FLOCK";

  g_debug ("FLOCK %" G_GINT64_MODIFIER "x", ino);

  xdp_reply_err (op, req, ENOSYS);
}

static void
xdp_fuse_init_cb (void                  *userdata,
                  struct fuse_conn_info *conn)
{
  XdpFuseOptions *fuse_opts = userdata;

  g_debug ("INIT");

  /* atomic_o_trunc: We handle O_TRUNC in create() */
  conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;

  if (fuse_opts->use_splice)
    {
      /* splice_read: use splice() to read from fuse pipe */
      conn->want |= FUSE_CAP_SPLICE_READ;
      /* splice_write: use splice() to write to fuse pipe */
      conn->want |= FUSE_CAP_SPLICE_WRITE;
      /* splice_move: move buffers from writing app to kernel during splice write */
      conn->want |= FUSE_CAP_SPLICE_MOVE;
    }
}

extern gboolean on_fuse_unmount (void *);

static void
xdp_fuse_destroy_cb (void *userdata)
{
  XdpFuseOptions *fuse_opts = userdata;

  g_debug ("DESTROY");

  /* Ensure we call this on the main thread */
  g_idle_add ((GSourceFunc) on_fuse_unmount, NULL);

  g_clear_pointer (&fuse_opts, g_free);
}

static struct fuse_lowlevel_ops xdp_fuse_oper = {
 .init         = xdp_fuse_init_cb,
 .destroy      = xdp_fuse_destroy_cb,
 .lookup       = xdp_fuse_lookup,
 .getattr      = xdp_fuse_getattr,
 .setattr      = xdp_fuse_setattr,
 .readdir      = xdp_fuse_readdir,
 .open         = xdp_fuse_open,
 .read         = xdp_fuse_read,
 .write        = xdp_fuse_write,
 .write_buf    = xdp_fuse_write_buf,
 .fsync        = xdp_fuse_fsync,
 .forget       = xdp_fuse_forget,
 .forget_multi = xdp_fuse_forget_multi,
 .releasedir   = xdp_fuse_releasedir,
 .release      = xdp_fuse_release,
 .opendir      = xdp_fuse_opendir,
 .fsyncdir     = xdp_fuse_fsyncdir,
 .create       = xdp_fuse_create,
 .unlink       = xdp_fuse_unlink,
 .rename       = xdp_fuse_rename,
 .access       = xdp_fuse_access,
 .readlink     = xdp_fuse_readlink,
 .rmdir        = xdp_fuse_rmdir,
 .mkdir        = xdp_fuse_mkdir,
 .symlink      = xdp_fuse_symlink,
 .link         = xdp_fuse_link,
 .flush        = xdp_fuse_flush,
 .statfs       = xdp_fuse_statfs,
 .setxattr     = xdp_fuse_setxattr,
 .getxattr     = xdp_fuse_getxattr,
 .listxattr    = xdp_fuse_listxattr,
 .removexattr  = xdp_fuse_removexattr,
 .getlk        = xdp_fuse_getlk,
 .setlk        = xdp_fuse_setlk,
 .flock        = xdp_fuse_flock,
 .fallocate    = xdp_fuse_fallocate,
};

typedef struct {
  GMutex lock;
  GCond cond;
  GError *error;
} XdpFuseThreadData;

static void
xdp_fuse_mainloop (struct fuse_session     *se,
                   struct fuse_loop_config *loop_config)
{
  const char *status;

  fuse_session_loop_mt (se, loop_config);

  status = getenv ("TEST_DOCUMENT_PORTAL_FUSE_STATUS");
  if (status)
    {
      GError *error = NULL;
      g_autoptr(GString) s = g_string_new ("");

      g_string_append (s, "ok");

      g_file_set_contents (status, s->str, -1, &error);
      g_assert_no_error (error);
    }
}

typedef struct fuse_args XdpAutoFuseArgs;
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (XdpAutoFuseArgs, fuse_opt_free_args);

static gpointer
xdp_fuse_thread (gpointer data)
{
  /* Options:
   *  auto_unmount: Tell fusermount to auto unmount if we die.
   */
  static char *fusermount_argv[] = {
    "xdp-fuse", "-osubtype=portal,fsname=portal,auto_unmount",
  };
  g_auto(XdpAutoFuseArgs) args =
    FUSE_ARGS_INIT (G_N_ELEMENTS (fusermount_argv), fusermount_argv);
  g_autoptr(GMutexLocker) session_locker = NULL;
  g_autoptr(GMutexLocker) locker = NULL;
  struct fuse_cmdline_opts opts = {0};
  struct fuse_loop_config loop_config = {0};
  XdpFuseThreadData *thread_data = data;
  XdpFuseOptions *fuse_opts = NULL;
  struct fuse_session *se;
  const char *path;

  locker = g_mutex_locker_new (&thread_data->lock);
  fuse_pthread = pthread_self ();

  g_cond_signal (&thread_data->cond);

  if (fuse_parse_cmdline (&args, &opts) != 0)
    {
      g_set_error (&thread_data->error, XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Impossible to parse command line");
      return NULL;
    }

  fuse_opts = g_new0 (XdpFuseOptions, 1);
#if HAVE_SPLICE
  fuse_opts->use_splice = TRUE;
#endif

  se = fuse_session_new (&args, &xdp_fuse_oper,
                         sizeof (xdp_fuse_oper), fuse_opts);
  if (se == NULL)
    {
      g_set_error (&thread_data->error, XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   "Can't create fuse session");
      g_clear_pointer (&fuse_opts, g_free);
      return NULL;
    }

  path = xdp_fuse_get_mountpoint ();
  if (fuse_session_mount (se, path) != 0)
    {
      fuse_session_destroy (se);
      g_set_error (&thread_data->error, XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   "Can't mount path %s", path);
      return NULL;
    }

  session = se;
  thread_data = NULL;
  g_clear_pointer (&locker, g_mutex_locker_free);

  loop_config.clone_fd = opts.clone_fd;
  loop_config.max_idle_threads = opts.max_idle_threads;
  thread_data = NULL;

  session_locker = g_mutex_locker_new (&G_LOCK_NAME (session));
  g_clear_pointer (&session_locker, g_mutex_locker_free);
  xdp_fuse_mainloop (session, &loop_config);

  session_locker = g_mutex_locker_new (&G_LOCK_NAME (session));
  fuse_session_unmount (se);
  fuse_session_destroy (se);
  session = NULL;

  return NULL;
}

gboolean
xdp_fuse_init (GError **error)
{
  g_autoptr(XdpDomain) by_app_domain = NULL;
  g_autoptr(XdpDomain) root_domain = NULL;
  XdpFuseThreadData thread_data = {0};
  struct statfs stfs;
  struct rlimit rl;
  struct stat st;
  const char *path;
  int statfs_res;

  my_uid = getuid ();
  my_gid = getgid ();

  all_inodes = g_hash_table_new_full (g_int64_hash, g_int64_equal, NULL, NULL);
  g_assert (open_files == NULL);

  root_domain = xdp_domain_new_root ();
  root_inode = xdp_inode_new (root_domain, NULL);
  by_app_domain = xdp_domain_new_by_app (root_inode);
  by_app_inode = xdp_inode_new (by_app_domain, NULL);

  physical_inodes =
    g_hash_table_new_full (devino_hash, devino_equal, NULL, NULL);

    /* Bump nr of filedescriptor limit to max */
  if (getrlimit (RLIMIT_NOFILE , &rl) == 0 &&
      rl.rlim_cur != rl.rlim_max)
    {
      rl.rlim_cur = rl.rlim_max;
      setrlimit (RLIMIT_NOFILE, &rl);
    }

  path = xdp_fuse_get_mountpoint ();

  if ((stat (path, &st) == -1 && errno == ENOTCONN) ||
      ((statfs_res = statfs (path, &stfs)) == -1 && errno == ENOTCONN) ||
      (statfs_res == 0 && stfs.f_type == 0x65735546 /* fuse */))
    {
      int count = 0;
      char *umount_argv[] = { "fusermount3", "-u", "-z", (char *) path, NULL };

      g_spawn_sync (NULL, umount_argv, NULL, G_SPAWN_SEARCH_PATH,
                    NULL, NULL, NULL, NULL, NULL, NULL);

      g_usleep (10000); /* 10ms */
      while (stat (path, &st) == -1 && count++ < 10)
        g_usleep (10000); /* 10ms */
    }

  if (g_mkdir_with_parents (path, 0700))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   "Unable to create dir %s", path);
      return FALSE;
    }

  g_mutex_init (&thread_data.lock);
  g_cond_init (&thread_data.cond);

  g_mutex_lock (&thread_data.lock);
  XDP_AUTOLOCK (session);
  fuse_thread = g_thread_new ("fuse mainloop", xdp_fuse_thread, &thread_data);

  while (session == NULL && thread_data.error == NULL)
    g_cond_wait (&thread_data.cond, &thread_data.lock);

  g_mutex_unlock (&thread_data.lock);
  g_cond_clear (&thread_data.cond);
  g_mutex_clear (&thread_data.lock);

  if (thread_data.error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&thread_data.error));
      return FALSE;
    }

  g_assert (session != NULL);

  XDP_AUTOLOCK (open_files);
  while (open_files)
    xdp_file_free (open_files->data);

  return TRUE;
}

void
xdp_fuse_exit (void)
{
  {
    XDP_AUTOLOCK (session);

    if (session)
      fuse_session_exit (session);

    if (fuse_pthread)
      pthread_kill (fuse_pthread, SIGHUP);
  }

  g_clear_pointer (&fuse_thread, g_thread_join);
  g_assert (session == NULL);
}

const char *
xdp_fuse_get_mountpoint (void)
{
  if (mount_path == NULL)
    mount_path = g_build_filename (g_get_user_runtime_dir (), "doc", NULL);
  return mount_path;
}

typedef struct {
  fuse_ino_t ino;
  char *filename;
} Invalidate;

/* Called with domain_inodes lock held, don't block */
static void
invalidate_doc_inode (XdpInode   *parent_inode,
                      const char *doc_id,
                      GArray     *invalidates)
{
  XdpInode *doc_inode = g_hash_table_lookup (parent_inode->domain->inodes, doc_id);
  Invalidate inval;

  if (doc_inode == NULL)
    return;

  inval.ino = xdp_inode_to_ino (doc_inode);
  inval.filename = NULL;
  g_array_append_val (invalidates, inval);

  inval.ino = xdp_inode_to_ino (parent_inode);
  inval.filename = g_strdup (doc_id);
  g_array_append_val (invalidates, inval);

  /* No need to invalidate doc children, we don't cache them */
}


/* Called when a apps permissions to see a document is changed,
   and with null opt_app_id when the doc is created/removed */
void
xdp_fuse_invalidate_doc_app (const char *doc_id,
                             const char *opt_app_id)
{
  g_autoptr(GArray) invalidates = NULL;
  XDP_AUTOLOCK (session);
  int i;

  /* This can happen if fuse is not initialized yet for the very
     first dbus message that activated the service */
  if (session == NULL)
    return;

  g_debug ("invalidate %s/%s", doc_id, opt_app_id ? opt_app_id : "*");

  invalidates = g_array_new (FALSE, FALSE, sizeof (Invalidate));

  G_LOCK (domain_inodes);
  if (opt_app_id != NULL)
    {
      XdpInode *app_inode = g_hash_table_lookup (by_app_inode->domain->inodes, opt_app_id);
      if (app_inode)
        invalidate_doc_inode (app_inode, doc_id, invalidates);
    }
  else
    {
      GHashTableIter iter;
      gpointer key, value;

      invalidate_doc_inode (root_inode, doc_id, invalidates);
      g_hash_table_iter_init (&iter, by_app_inode->domain->inodes);
      while (g_hash_table_iter_next (&iter, &key, &value))
        invalidate_doc_inode ((XdpInode *)value, doc_id, invalidates);
    }

  G_UNLOCK (domain_inodes);

  for (i = 0; i < invalidates->len; i++)
    {
      Invalidate *invalidate = &g_array_index (invalidates, Invalidate, i);

      if (invalidate->filename)
        {
          fuse_lowlevel_notify_inval_entry (session, invalidate->ino,
                                            invalidate->filename, strlen (invalidate->filename));
          g_free (invalidate->filename);
        }
      else
        fuse_lowlevel_notify_inval_inode (session, invalidate->ino, 0, 0);
    }
}

char *
xdp_fuse_lookup_id_for_inode (ino_t      ino,
                              gboolean   directory,
                              char     **real_path_out)
{
  g_autoptr(XdpDomain) domain = NULL;
  g_autoptr(XdpPhysicalInode) physical = NULL;
  DevIno file_devino;
  struct stat buf;

  if (real_path_out)
    *real_path_out = NULL;

  G_LOCK (all_inodes);
  {
    XdpInode *inode = g_hash_table_lookup (all_inodes, &ino);
    if (inode)
      {
        /* We're not allowed to resurrect the inode here, but we can get the data while in the lock */
        domain = xdp_domain_ref (inode->domain);
        if (inode->physical)
          physical = xdp_physical_inode_ref (inode->physical);
      }
  }
  G_UNLOCK (all_inodes);

  if (domain == NULL)
    return NULL;

  if (domain->type != XDP_DOMAIN_DOCUMENT)
    return NULL;

  if (physical == NULL)
    return NULL;

  file_devino = physical->backing_devino;

  if (!xdp_document_domain_is_dir (domain))
    {
      g_autofree char *main_path = g_build_filename (domain->doc_path, domain->doc_file, NULL);

      /* file document */

      if (directory)
        return NULL;

      /* Only return for main file */
      if (lstat (main_path, &buf) == 0 &&
          buf.st_dev == file_devino.dev &&
          buf.st_ino == file_devino.ino)
        return g_strdup (domain->doc_id);
    }
  else
    {
      /* directory document */

      /* Only return entire doc for main dir */
      if (file_devino.dev == domain->doc_dir_device &&
          file_devino.ino == domain->doc_dir_inode)
        return g_strdup (domain->doc_id);

      /* But maybe its a subfile of the document */
      if (real_path_out && xdp_fuse_get_real_path (physical, real_path_out))
        return g_strdup (domain->doc_id);
    }

  return NULL;
}
