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
 *       Matthias Clasen <mclasen@redhat.com>
 *       Hubert Figuière <hub@figuiere.net>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "file-transfer.h"
#include "src/xdp-utils.h"
#include "document-portal-dbus.h"
#include "document-enums.h"
#include "document-portal.h"
#include "document-portal-fuse.h"

static XdpDbusFileTransfer *file_transfer;

typedef struct
{
  char *path;
  int parent_dev;
  int parent_ino;
  gboolean is_dir;
} ExportedFile;

static void
exported_file_free (gpointer data)
{
  ExportedFile *file = data;

  g_free (file->path);
  g_free (file);
}

typedef struct
{
  GObject object;
  GMutex mutex;

  GPtrArray *files;
  gboolean writable;
  gboolean autostop;
  char *key;
  char *sender;
  XdpAppInfo *app_info;
} FileTransfer;

typedef struct
{
  GObjectClass parent_class;
} FileTransferClass;

static GType file_transfer_get_type (void);

G_DEFINE_TYPE (FileTransfer, file_transfer, G_TYPE_OBJECT)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FileTransfer, g_object_unref);

static void
file_transfer_init (FileTransfer *transfer)
{
  g_mutex_init (&transfer->mutex);
}

static void
file_transfer_finalize (GObject *object)
{
  FileTransfer *transfer = (FileTransfer *)object;

  g_mutex_clear (&transfer->mutex);
  xdp_app_info_unref (transfer->app_info);
  g_ptr_array_unref (transfer->files);
  g_free (transfer->key);
  g_free (transfer->sender);

  G_OBJECT_CLASS (file_transfer_parent_class)->finalize (object);
}

static void
file_transfer_class_init (FileTransferClass *class)
{
  G_OBJECT_CLASS (class)->finalize = file_transfer_finalize;
}

static inline void
auto_unlock_unref_helper (FileTransfer **transfer)
{
  if (!*transfer)
    return;

  g_mutex_unlock (&(*transfer)->mutex);
  g_object_unref (*transfer);
}

static inline FileTransfer *
auto_lock_helper (FileTransfer *transfer)
{
  if (transfer)
    g_mutex_lock (&transfer->mutex);
  return transfer;
}

#define TRANSFER_AUTOLOCK_UNREF(transfer) \
  G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_unref_helper))) \
  FileTransfer * G_PASTE (auto_unlock_unref, __LINE__) = \
    auto_lock_helper (transfer);

G_LOCK_DEFINE (transfers);
static GHashTable *transfers;

static FileTransfer *
lookup_transfer (const char *key)
{
  FileTransfer *transfer;

  G_LOCK (transfers);
  transfer = (FileTransfer *)g_hash_table_lookup (transfers, key);
  if (transfer)
    g_object_ref (transfer);
  G_UNLOCK (transfers);

  return transfer;
}

static FileTransfer *
file_transfer_start (XdpAppInfo *app_info,
                     const char *sender,
                     gboolean    writable,
                     gboolean    autostop)
{
  FileTransfer *transfer;

  transfer = g_object_new (file_transfer_get_type (), NULL);

  transfer->app_info = xdp_app_info_ref (app_info);
  transfer->sender = g_strdup (sender);
  transfer->writable = writable;
  transfer->autostop = autostop;
  transfer->files = g_ptr_array_new_with_free_func (exported_file_free);

  G_LOCK (transfers);
  do {
    guint64 key;
    g_free (transfer->key);
    key = g_random_int ();
    key = (key << 32) | g_random_int ();
    transfer->key = g_strdup_printf ("%lu", key);
  }
  while (g_hash_table_contains (transfers, transfer->key));
  g_hash_table_insert (transfers, transfer->key, g_object_ref (transfer));
  G_UNLOCK (transfers);

  g_debug ("start file transfer owned by '%s' (%s)",
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  return transfer;
}

static gboolean
stop (gpointer data)
{
  FileTransfer *transfer = data;

  g_object_unref (transfer);

  return G_SOURCE_REMOVE;
}

static void
file_transfer_stop (FileTransfer *transfer)
{
  GDBusConnection *bus;

  g_debug ("stop file transfer owned by '%s' (%s)",
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  bus = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (file_transfer));
  g_dbus_connection_emit_signal (bus,
                                 transfer->sender,
                                 "/org/freedesktop/portal/documents",
                                 "org.freedesktop.portal.FileTransfer",
                                 "TransferClosed",
                                 g_variant_new ("(s)", transfer->key),
                                 NULL);

  G_LOCK (transfers);
  g_hash_table_steal (transfers, transfer->key);
  G_UNLOCK (transfers);

  g_idle_add (stop, transfer);
}

static void
file_transfer_add_file (FileTransfer *transfer,
                        const char *path,
                        struct stat *st_buf,
                        struct stat *parent_st_buf)
{
  ExportedFile *file;

  file = g_new (ExportedFile, 1);
  file->path = g_strdup (path);
  file->is_dir = S_ISDIR (st_buf->st_mode);
  file->parent_dev = parent_st_buf->st_dev;
  file->parent_ino = parent_st_buf->st_ino;

  g_ptr_array_add (transfer->files, file);
}

static char **
file_transfer_execute (FileTransfer *transfer,
                       XdpAppInfo *target_app_info,
                       GError **error)
{
  DocumentAddFullFlags common_flags;
  DocumentPermissionFlags perms;
  const char *target_app_id;
  int n_fds;
  g_autofree int *fds = NULL;
  g_autofree int *parent_devs = NULL;
  g_autofree int *parent_inos = NULL;
  g_autofree DocumentAddFullFlags *documents_flags = NULL;
  int i;
  g_auto(GStrv) ids = NULL;
  char **files = NULL;

  g_debug ("retrieve %d files for %s from file transfer owned by '%s' (%s)",
           transfer->files->len,
           xdp_app_info_get_id (target_app_info),
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  /* if the target is unsandboxed, just return the files as-is */
  if (xdp_app_info_is_host (target_app_info))
    {
      files = g_new (char *, transfer->files->len + 1);
      for (i = 0; i < transfer->files->len; i++)
        {
          ExportedFile *file = (ExportedFile*)g_ptr_array_index (transfer->files, i);
          files[i] = g_strdup (file->path);
        }
      files[i] = NULL;
      return files;
    }

  common_flags = DOCUMENT_ADD_FLAGS_REUSE_EXISTING | DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP;

  perms = DOCUMENT_PERMISSION_FLAGS_READ;
  if (transfer->writable)
    perms |= DOCUMENT_PERMISSION_FLAGS_WRITE;

  target_app_id = xdp_app_info_get_id (target_app_info);

  n_fds = transfer->files->len;
  fds = g_new (int, n_fds);
  parent_devs = g_new (int, n_fds);
  parent_inos = g_new (int, n_fds);
  documents_flags = g_new (DocumentAddFullFlags, n_fds);
  for (i = 0; i < n_fds; i++)
    {
      ExportedFile *file = (ExportedFile*)g_ptr_array_index (transfer->files, i);

      fds[i] = open (file->path, O_PATH | O_CLOEXEC);
      if (fds[i] == -1)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "File transfer %s failed", transfer->key);
          for (; i > 0; i--)
            close (fds[i - 1]);
          return NULL;
        }

      documents_flags[i] = common_flags | (file->is_dir ? DOCUMENT_ADD_FLAGS_DIRECTORY : 0);
      parent_devs[i] = file->parent_dev;
      parent_inos[i] = file->parent_ino;
    }

  ids = document_add_full (fds, parent_devs, parent_inos, documents_flags, n_fds, transfer->app_info, target_app_id, perms, error);

  for (i = 0; i < n_fds; i++)
    close (fds[i]);

  if (ids)
    {
      const char *mountpoint = xdp_fuse_get_mountpoint ();
      files = g_new (char *, n_fds + 1);
      for (i = 0; i < n_fds; i++)
        {
          ExportedFile *file = (ExportedFile *) g_ptr_array_index (transfer->files, i);

          if (ids[i][0] == '\0')
            files[i] = g_strdup (file->path);
          else
            {
              g_autofree char *name = g_path_get_basename (file->path);
              files[i] = g_build_filename (mountpoint, ids[i], name, NULL);
            }
        }
      files[n_fds] = NULL;
    }

  return files;
}

static void
start_transfer (GDBusMethodInvocation *invocation,
                GVariant *parameters,
                XdpAppInfo *app_info)
{
  g_autoptr(GVariant) options = NULL;
  g_autoptr(FileTransfer) transfer = NULL;
  gboolean writable;
  gboolean autostop;
  const char *sender;

  g_variant_get (parameters, "(@a{sv})", &options);
  if (!g_variant_lookup (options, "writable", "b", &writable))
    writable = FALSE;

  if (!g_variant_lookup (options, "autostop", "b", &autostop))
    autostop = TRUE;

  sender = g_dbus_method_invocation_get_sender (invocation);

  transfer = file_transfer_start (app_info, sender, writable, autostop);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", transfer->key));
}

static void
add_files (GDBusMethodInvocation *invocation,
           GVariant *parameters,
           XdpAppInfo *app_info)
{
  FileTransfer *transfer;
  const char *key;
  g_autoptr(GVariant) options = NULL;
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autoptr(GVariantIter) iter = NULL;
  int fd_id;
  const int *fds;
  int n_fds;

  g_variant_get (parameters, "(&sah@a{sv})", &key, &iter, &options);

  transfer = lookup_transfer (key);
  if (transfer == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  TRANSFER_AUTOLOCK_UNREF (transfer);

  if (strcmp (transfer->sender, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  if (fd_list == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid transfer");
      return;
    }

  fds = g_unix_fd_list_peek_fds (fd_list, &n_fds);

  g_debug ("add %d files to file transfer owned by '%s' (%s)", n_fds,
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  while (g_variant_iter_next (iter, "h", &fd_id))
    {
      int fd = -1;
      g_autofree char *path = NULL;
      gboolean fd_is_writable;
      struct stat st_buf;
      struct stat parent_st_buf;

      if (fd_id < n_fds)
        fd = fds[fd_id];

      if (fd == -1)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_ACCESS_DENIED,
                                                 "Invalid transfer");
          return;
        }

      if (!validate_fd (fd, app_info, VALIDATE_FD_FILE_TYPE_ANY, &st_buf, &parent_st_buf, &path, &fd_is_writable, NULL) ||
          (transfer->writable && !fd_is_writable))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                                 "Can't export file");
          return;
        }

      file_transfer_add_file (transfer, path, &st_buf, &parent_st_buf);
    }

  g_dbus_method_invocation_return_value (invocation, NULL);
}


static void
retrieve_files (GDBusMethodInvocation *invocation,
                GVariant *parameters,
                XdpAppInfo *app_info)
{
  const char *key;
  FileTransfer *transfer;
  g_auto(GStrv) files = NULL;
  g_autoptr(GError) error = NULL;

  g_variant_get (parameters, "(&s@a{sv})", &key, NULL);

  transfer = lookup_transfer (key);
  if (transfer == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  TRANSFER_AUTOLOCK_UNREF (transfer);

  files = file_transfer_execute (transfer, app_info, &error);
  if (files == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(^as)", files));

  if (transfer->autostop)
    file_transfer_stop (transfer);
}

static void
stop_transfer (GDBusMethodInvocation *invocation,
                GVariant *parameters,
                XdpAppInfo *app_info)
{
  const char *key;
  FileTransfer *transfer;

  g_variant_get (parameters, "(&s)", &key);

  transfer = lookup_transfer (key);
  if (transfer == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  TRANSFER_AUTOLOCK_UNREF (transfer);

  file_transfer_stop (transfer);

  g_dbus_method_invocation_return_value (invocation, NULL);
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

GDBusInterfaceSkeleton *
file_transfer_create (void)
{
  file_transfer = xdp_dbus_file_transfer_skeleton_new ();

  g_signal_connect_swapped (file_transfer, "handle-start-transfer", G_CALLBACK (handle_method), start_transfer);
  g_signal_connect_swapped (file_transfer, "handle-add-files", G_CALLBACK (handle_method), add_files);
  g_signal_connect_swapped (file_transfer, "handle-retrieve-files", G_CALLBACK (handle_method), retrieve_files);
  g_signal_connect_swapped (file_transfer, "handle-stop-transfer", G_CALLBACK (handle_method), stop_transfer);

  xdp_dbus_file_transfer_set_version (XDP_DBUS_FILE_TRANSFER (file_transfer), 1);

  transfers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

  return G_DBUS_INTERFACE_SKELETON (file_transfer);
}

void
stop_file_transfers_in_thread_func (GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
  const char *sender = (const char *)task_data;
  GHashTableIter iter;
  FileTransfer *transfer;

  G_LOCK (transfers);
  if (transfers)
    {
      g_hash_table_iter_init (&iter, transfers);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&transfer))
        {
          if (strcmp (sender, transfer->sender) == 0)
            {
              g_print ("removing transfer %s for dead peer %s\n", transfer->key, transfer->sender);
              g_hash_table_iter_remove (&iter);
            }
        }
    }
  G_UNLOCK (transfers);
  g_task_return_boolean (task, TRUE);
}

void
stop_file_transfers_for_sender (const char *sender)
{
  GTask *task;

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_strdup (sender), g_free);
  g_task_run_in_thread (task, stop_file_transfers_in_thread_func);
  g_object_unref (task);
}
