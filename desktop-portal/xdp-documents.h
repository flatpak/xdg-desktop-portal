/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gdesktopappinfo.h>
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
                             GDesktopAppInfo   *app_info,
                             XdpDocumentFlags   flags,
                             GError           **error);

char *xdp_get_real_path_for_doc_id (const char *doc_id);

typedef enum {
  XDP_RESOLVE_DOCUMENT_TO_FILE,
  XDP_RESOLVE_DOCUMENT_TO_DIRECTORY
} XdpResolveDocumentStrategy;

char * xdp_resolve_document_portal_path (const char                 *path,
                                         XdpResolveDocumentStrategy  strategy);
