#include "config.h"
#include <string.h>
#include <errno.h>
#include <gio/gio.h>
#include "document-store.h"

const char **
xdg_unparse_permissions (DocumentPermissionFlags permissions)
{
  GPtrArray *array;

  array = g_ptr_array_new ();

  if (permissions & DOCUMENT_PERMISSION_FLAGS_READ)
    g_ptr_array_add (array, "read");
  if (permissions & DOCUMENT_PERMISSION_FLAGS_WRITE)
    g_ptr_array_add (array, "write");
  if (permissions & DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS)
    g_ptr_array_add (array, "grant-permissions");
  if (permissions & DOCUMENT_PERMISSION_FLAGS_DELETE)
    g_ptr_array_add (array, "delete");

  g_ptr_array_add (array, NULL);
  return (const char **) g_ptr_array_free (array, FALSE);
}

DocumentPermissionFlags
xdp_parse_permissions (const char **permissions,
                       GError     **error)
{
  DocumentPermissionFlags perms;
  int i;

  perms = 0;
  for (i = 0; permissions[i]; i++)
    {
      if (strcmp (permissions[i], "read") == 0)
        perms |= DOCUMENT_PERMISSION_FLAGS_READ;
      else if (strcmp (permissions[i], "write") == 0)
        perms |= DOCUMENT_PERMISSION_FLAGS_WRITE;
      else if (strcmp (permissions[i], "grant-permissions") == 0)
        perms |= DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS;
      else if (strcmp (permissions[i], "delete") == 0)
        perms |= DOCUMENT_PERMISSION_FLAGS_DELETE;
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "No such permission");
          return 0;
        }
    }

  return perms;
}

DocumentPermissionFlags
document_entry_get_permissions_by_app_id (PermissionDbEntry *entry,
                                          const char        *app_id)
{
  g_autofree const char **permissions = NULL;

  if (strcmp (app_id, "") == 0)
    return DOCUMENT_PERMISSION_FLAGS_ALL;

  permissions = permission_db_entry_list_permissions (entry, app_id);
  return xdp_parse_permissions (permissions, NULL);
}

DocumentPermissionFlags
document_entry_get_permissions (PermissionDbEntry *entry,
                                XdpAppInfo        *app_info)
{
  g_autofree const char **permissions = NULL;
  const char *app_id = xdp_app_info_get_id (app_info);

  if (xdp_app_info_is_host (app_info))
    return DOCUMENT_PERMISSION_FLAGS_ALL;

  return document_entry_get_permissions_by_app_id (entry, app_id);
}

gboolean
document_entry_has_permissions_by_app_id (PermissionDbEntry       *entry,
                                          const char              *app_id,
                                          DocumentPermissionFlags  perms)
{
  DocumentPermissionFlags current_perms;

  current_perms = document_entry_get_permissions_by_app_id (entry, app_id);

  return (current_perms & perms) == perms;
}

gboolean
document_entry_has_permissions (PermissionDbEntry       *entry,
                                XdpAppInfo              *app_info,
                                DocumentPermissionFlags  perms)
{
  DocumentPermissionFlags current_perms;

  current_perms = document_entry_get_permissions (entry, app_info);

  return (current_perms & perms) == perms;
}

char *
xdp_name_from_id (guint32 doc_id)
{
  return g_strdup_printf ("%x", doc_id);
}

const char *
document_entry_get_path (PermissionDbEntry *entry)
{
  g_autoptr(GVariant) v = permission_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 0);
  return g_variant_get_bytestring (c);
}

char *
document_entry_dup_basename (PermissionDbEntry *entry)
{
  const char *path = document_entry_get_path (entry);

  return g_path_get_basename (path);
}

char *
document_entry_dup_dirname (PermissionDbEntry *entry)
{
  const char *path = document_entry_get_path (entry);

  return g_path_get_dirname (path);
}

guint64
document_entry_get_device (PermissionDbEntry *entry)
{
  g_autoptr(GVariant) v = permission_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 1);
  return g_variant_get_uint64 (c);
}

guint64
document_entry_get_inode (PermissionDbEntry *entry)
{
  g_autoptr(GVariant) v = permission_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 2);
  return g_variant_get_uint64 (c);
}

guint32
document_entry_get_flags (PermissionDbEntry *entry)
{
  g_autoptr(GVariant) v = permission_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 3);
  return g_variant_get_uint32 (c);
}
