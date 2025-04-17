/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "xdp-app-info.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"
#include "xdp-documents.h"
#include "document-enums.h"

static XdpDbusDocuments *documents = NULL;
static char *documents_mountpoint = NULL;

gboolean
xdp_init_document_proxy (GDBusConnection  *connection,
                         GError          **error)
{
  g_autoptr(GError) local_error = NULL;

  documents = xdp_dbus_documents_proxy_new_sync (connection, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);
  if (!documents)
    return FALSE;

  if (!xdp_dbus_documents_call_get_mount_point_sync (documents,
                                                     &documents_mountpoint,
                                                     NULL, &local_error))
    {
      g_warning ("Document portal fuse mount point unknown: %s",
                 local_error->message);
    }

  xdp_set_documents_mountpoint (documents_mountpoint);

  return TRUE;
}

char *
xdp_register_document (const char        *uri,
                       const char        *app_id,
                       XdpDocumentFlags   flags,
                       GError           **error)
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

  if (flags & XDP_DOCUMENT_FLAG_FOR_SAVE)
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
  if ((flags & XDP_DOCUMENT_FLAG_WRITABLE) || (flags & XDP_DOCUMENT_FLAG_FOR_SAVE))
    permissions[i++] = "write";
  permissions[i++] = "grant-permissions";
  if (flags & XDP_DOCUMENT_FLAG_DELETABLE)
    permissions[i++] = "delete";
  permissions[i++] = NULL;

  version = xdp_dbus_documents_get_version (documents);
  full_flags = DOCUMENT_ADD_FLAGS_REUSE_EXISTING | DOCUMENT_ADD_FLAGS_PERSISTENT | DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP;
  if (flags & XDP_DOCUMENT_FLAG_DIRECTORY)
    full_flags |= DOCUMENT_ADD_FLAGS_DIRECTORY;

  if (flags & XDP_DOCUMENT_FLAG_FOR_SAVE)
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
      return g_filename_to_uri (doc_path, NULL, error);
    }

  doc_path = g_build_filename (documents_mountpoint, doc_id, basename, NULL);
  return g_filename_to_uri (doc_path, NULL, error);
}

char *
xdp_get_real_path_for_doc_path (const char *path,
                                XdpAppInfo *app_info)
{
  g_autofree char *doc_id = NULL;
  gboolean ret = FALSE;
  g_autoptr(GError) error = NULL;

  if (xdp_app_info_is_host (app_info))
    return g_strdup (path);

  ret = xdp_dbus_documents_call_lookup_sync (documents, path, &doc_id, NULL, &error);
  if (!ret)
    {
      g_debug ("document portal error for path '%s': %s", path, error->message);
      return g_strdup (path);
    }

  if (!g_strcmp0 (doc_id, ""))
    {
      g_debug ("document portal returned empty doc id for path '%s'", path);
      return g_strdup (path);
    }

  return xdp_get_real_path_for_doc_id (doc_id);
}

char *
xdp_get_real_path_for_doc_id (const char *doc_id)
{
  gboolean ret = FALSE;
  char *real_path = NULL;
  g_autoptr (GError) error = NULL;

  ret = xdp_dbus_documents_call_info_sync (documents, doc_id, &real_path, NULL, NULL, &error);
  if (!ret)
    {
      g_debug ("document portal error for doc id '%s': %s", doc_id, error->message);
      return NULL;
    }

  return real_path;
}

/* Get the document id from the path, if there's any.
 * Returns TRUE when path seems to point to the documents
 * storage. Also returns the guessed doc id and suffix path
 * without the dir name immediately following the doc id.
 */
static gboolean
xdp_looks_like_document_portal_path (const char  *path,
                                     char       **docid_out,
                                     char       **suffix_path_out)
{
  g_autofree char *docid = NULL;
  g_autofree char *suffix_path = NULL;
  char *p, *q;

  if (!g_str_has_prefix (path, g_get_user_runtime_dir ()))
    return FALSE;

  p = strstr (path, "/doc/");
  if (!p)
    return FALSE;

  p += strlen ("/doc/");
  q = strchr (p, '/');

  if (q)
    {
      docid = g_strndup (p, q - p);

      /* The mapping from doc path to the host path already provides the dir name,
       * so we can omit it from the subpath.
       */
      q = strchr (++q, '/');
      if (q)
        suffix_path = g_strdup (q);
    }
  else
    {
      docid = g_strdup (p);
    }

  if (docid[0] == '\0')
    return FALSE;

  if (docid_out)
    *docid_out = g_steal_pointer (&docid);
  if (suffix_path_out)
    *suffix_path_out = g_steal_pointer (&suffix_path);
  return TRUE;
}

char *
xdp_resolve_document_portal_path (const char *path)
{
  g_autofree char *docid = NULL;
  g_autofree char *suffix_path = NULL;
  g_autofree char *host_path = NULL;

  if (!xdp_looks_like_document_portal_path (path, &docid, &suffix_path))
    return g_strdup (path);

  host_path = xdp_get_real_path_for_doc_id (docid);
  if (!host_path)
    return g_strdup (path);

  return g_strconcat (host_path, suffix_path, NULL);
}
