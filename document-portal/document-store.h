#pragma once

#include <gio/gio.h>
#include "permission-db.h"
#include "document-enums.h"
#include "src/xdp-utils.h"

G_BEGIN_DECLS

#define DOCUMENT_ENTRY_FLAG_UNIQUE (1 << 0)
#define DOCUMENT_ENTRY_FLAG_TRANSIENT (1 << 1)
#define DOCUMENT_ENTRY_FLAG_DIRECTORY (1 << 2)

const char **      xdg_unparse_permissions (DocumentPermissionFlags permissions);
DocumentPermissionFlags xdp_parse_permissions (const char **permissions,
                                               GError     **error);

DocumentPermissionFlags document_entry_get_permissions_by_app_id (PermissionDbEntry *entry,
                                                                  const char        *app_id);
DocumentPermissionFlags document_entry_get_permissions (PermissionDbEntry *entry,
                                                        XdpAppInfo        *app_info);
gboolean           document_entry_has_permissions (PermissionDbEntry       *entry,
                                                   XdpAppInfo              *app_info,
                                                   DocumentPermissionFlags  perms);
gboolean           document_entry_has_permissions_by_app_id (PermissionDbEntry       *entry,
                                                             const char              *app_id,
                                                             DocumentPermissionFlags  perms);
const char *       document_entry_get_path (PermissionDbEntry *entry);
char *             document_entry_dup_basename (PermissionDbEntry *entry);
char *             document_entry_dup_dirname (PermissionDbEntry *entry);
guint64            document_entry_get_device (PermissionDbEntry *entry);
guint64            document_entry_get_inode (PermissionDbEntry *entry);
guint32            document_entry_get_flags (PermissionDbEntry *entry);

char *  xdp_name_from_id (guint32 doc_id);


G_END_DECLS
