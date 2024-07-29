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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <gio/gio.h>

typedef enum {
  DOCUMENT_FLAG_NONE      = 0,
  DOCUMENT_FLAG_FOR_SAVE  = (1 << 0),
  DOCUMENT_FLAG_WRITABLE  = (1 << 1),
  DOCUMENT_FLAG_DIRECTORY = (1 << 2),
  DOCUMENT_FLAG_DELETABLE = (1 << 3),
} DocumentFlags;

gboolean init_document_proxy (GDBusConnection  *connection,
                              GError          **error);

char *register_document (const char *uri,
                         const char *app_id,
                         DocumentFlags flags,
                         GError **error);

char *get_real_path_for_doc_path (const char *path,
                                  XdpAppInfo *app_info);

char *get_real_path_for_doc_id (const char *doc_id);
