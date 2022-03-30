/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "xdp-dbus.h"
#include "xdp-utils.h"
#include "document-enums.h"

static XdpDbusDocuments *documents = NULL;
static char *documents_mountpoint = NULL;

void
init_document_proxy (GDBusConnection *connection)
{
  documents = xdp_dbus_documents_proxy_new_sync (connection, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, NULL);
  xdp_dbus_documents_call_get_mount_point_sync (documents,
                                                &documents_mountpoint,
                                                NULL, NULL);
  xdp_set_documents_mountpoint (documents_mountpoint);
}

char *
register_document (const char *uri,
                   const char *app_id,
                   gboolean for_save,
                   gboolean writable,
                   gboolean directory,
                   GError **error)
{
  g_autofree char *doc_id = NULL;
  g_auto(GStrv) doc_ids = NULL;
  g_autofree char *path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *dirname = NULL;
  GUnixFDList *fd_list = NULL;
  int fd, fd_in;
  g_autoptr(GFile) file = NULL;
  gboolean ret = FALSE;
  const char *permissions[5];
  g_autofree char *doc_path = NULL;
  int i;
  int version;
  gboolean handled_permissions = FALSE;
  DocumentAddFullFlags full_flags;

  g_return_val_if_fail (app_id != NULL && *app_id != '\0', NULL);

  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  basename = g_path_get_basename (path);
  dirname = g_path_get_dirname (path);

  if (for_save)
    fd = open (dirname, O_PATH | O_CLOEXEC);
  else
    fd = open (path, O_PATH | O_CLOEXEC);

  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to open %s", uri);
      return NULL;
    }
  fd_list = g_unix_fd_list_new ();
  fd_in = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (fd_in == -1)
    return NULL;

  i = 0;
  permissions[i++] = "read";
  if (writable || for_save)
    permissions[i++] = "write";
  permissions[i++] = "grant-permissions";
  permissions[i++] = NULL;

  version = xdp_dbus_documents_get_version (documents);
  full_flags = DOCUMENT_ADD_FLAGS_REUSE_EXISTING | DOCUMENT_ADD_FLAGS_PERSISTENT | DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP;
  if (directory)
    full_flags |= DOCUMENT_ADD_FLAGS_DIRECTORY;

  if (for_save)
    {
      if (version >= 3)
        {
          ret = xdp_dbus_documents_call_add_named_full_sync (documents,
                                                             g_variant_new_handle (fd_in),
                                                             basename,
                                                             full_flags,
                                                             app_id,
                                                             permissions,
                                                             fd_list,
                                                             &doc_id,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             error);
          handled_permissions = TRUE;
        }
      else
        ret = xdp_dbus_documents_call_add_named_sync (documents,
                                                      g_variant_new_handle (fd_in),
                                                      basename,
                                                      TRUE,
                                                      TRUE,
                                                      fd_list,
                                                      &doc_id,
                                                      NULL,
                                                      NULL,
                                                      error);
    }
  else
    {
      if (version >= 2)
        {
          ret = xdp_dbus_documents_call_add_full_sync (documents,
                                                       g_variant_new_fixed_array (G_VARIANT_TYPE_HANDLE, &fd_in, 1, sizeof (gint32)),
                                                       full_flags,
                                                       app_id,
                                                       permissions,
                                                       fd_list,
                                                       &doc_ids,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       error);
          handled_permissions = TRUE;
        }
      else
        ret = xdp_dbus_documents_call_add_sync (documents,
                                                g_variant_new_handle (fd_in),
                                                TRUE,
                                                TRUE,
                                                fd_list,
                                                &doc_id,
                                                NULL,
                                                NULL,
                                                error);
    }

  g_object_unref (fd_list);

  if (!ret)
    return NULL;

  if (doc_ids && doc_ids[0]) {
      doc_id = g_strdup (doc_ids[0]);
  }

  if (!handled_permissions)
    {
      if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                           doc_id,
                                                           app_id,
                                                           permissions,
                                                           NULL,
                                                           error))
        return NULL;
    }

  if (!g_strcmp0 (doc_id, ""))
    {
      doc_path = g_build_filename (path, NULL);
      return g_filename_to_uri (doc_path, NULL, NULL);
    }

  doc_path = g_build_filename (documents_mountpoint, doc_id, basename, NULL);
  return g_filename_to_uri (doc_path, NULL, NULL);
}

char *
get_real_path_for_doc_path (const char *path,
                            XdpAppInfo *app_info)
{
  g_autofree char *doc_id = NULL;
  gboolean ret = FALSE;
  char *real_path = NULL;

  if (xdp_app_info_is_host (app_info))
    return g_strdup (path);

  ret = xdp_dbus_documents_call_lookup_sync (documents, path, &doc_id,
                                             NULL, NULL);
  if (!ret)
    return g_strdup (path);

  if (!g_strcmp0 (doc_id, ""))
    return g_strdup (path);

  ret = xdp_dbus_documents_call_info_sync (documents, doc_id, &real_path,
                                           NULL, NULL, NULL);
  if (!ret)
    return g_strdup (path);

  return real_path;
}
