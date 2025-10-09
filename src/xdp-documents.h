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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <gio/gio.h>

#include "xdp-types.h"

typedef enum {
  XDP_DOCUMENT_FLAG_NONE      = 0,
  XDP_DOCUMENT_FLAG_FOR_SAVE  = (1 << 0),
  XDP_DOCUMENT_FLAG_WRITABLE  = (1 << 1),
  XDP_DOCUMENT_FLAG_DIRECTORY = (1 << 2),
  XDP_DOCUMENT_FLAG_DELETABLE = (1 << 3),
} XdpDocumentFlags;

gboolean xdp_init_document_proxy (GDBusConnection  *connection,
                                  GError          **error);

char *xdp_register_document (const char        *uri,
                             const char        *app_id,
                             XdpDocumentFlags   flags,
                             GError           **error);

char *xdp_get_real_path_for_doc_path (const char *path,
                                      XdpAppInfo *app_info);

char *xdp_get_real_path_for_doc_id (const char *doc_id);

char * xdp_resolve_document_portal_path (const char *path);
