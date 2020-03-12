#ifndef XDP_UTIL_H
#define XDP_UTIL_H

#include <gio/gio.h>
#include "permission-db.h"
#include "document-enums.h"

G_BEGIN_DECLS

#define DOCUMENT_ENTRY_FLAG_UNIQUE (1 << 0)
#define DOCUMENT_ENTRY_FLAG_TRANSIENT (1 << 1)
#define DOCUMENT_ENTRY_FLAG_DIRECTORY (1 << 2)

const char **      xdg_unparse_permissions (DocumentPermissionFlags permissions);
DocumentPermissionFlags xdp_parse_permissions (const char **permissions,
                                               GError     **error);

DocumentPermissionFlags document_entry_get_permissions (PermissionDbEntry *entry,
                                                   const char     *app_id);
gboolean           document_entry_has_permissions (PermissionDbEntry    *entry,
                                                   const char        *app_id,
                                                   DocumentPermissionFlags perms);
const char *       document_entry_get_path (PermissionDbEntry *entry);
char *             document_entry_dup_basename (PermissionDbEntry *entry);
char *             document_entry_dup_dirname (PermissionDbEntry *entry);
guint64            document_entry_get_device (PermissionDbEntry *entry);
guint64            document_entry_get_inode (PermissionDbEntry *entry);
guint32            document_entry_get_flags (PermissionDbEntry *entry);

char *  xdp_name_from_id (guint32 doc_id);


G_END_DECLS

#endif /* XDP_UTIL_H */
