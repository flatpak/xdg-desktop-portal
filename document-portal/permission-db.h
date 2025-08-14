/* permission-db.h
 *
 * Copyright © 2015 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef PERMISSION_DB_H
#define PERMISSION_DB_H

#include <string.h>

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct PermissionDb       PermissionDb;
typedef struct _PermissionDbEntry PermissionDbEntry;

#define PERMISSION_TYPE_DB (permission_db_get_type ())
#define PERMISSION_DB(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), PERMISSION_TYPE_DB, PermissionDb))
#define PERMISSION_IS_DB(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PERMISSION_TYPE_DB))

GType permission_db_get_type (void);

PermissionDb *     permission_db_new (const char *path,
                                      gboolean    fail_if_not_found,
                                      GError    **error);
char **        permission_db_list_ids (PermissionDb *self);
char **        permission_db_list_apps (PermissionDb *self);
char **        permission_db_list_ids_by_app (PermissionDb  *self,
                                              const char *app_permissions_id);
char **        permission_db_list_ids_by_value (PermissionDb *self,
                                                GVariant  *data);
PermissionDbEntry *permission_db_lookup (PermissionDb  *self,
                                   const char *id);
GString *      permission_db_print_string (PermissionDb *self,
                                           GString   *string);
char *         permission_db_print (PermissionDb *self);

gboolean       permission_db_is_dirty (PermissionDb *self);
void           permission_db_set_entry (PermissionDb      *self,
                                        const char     *id,
                                        PermissionDbEntry *entry);
void           permission_db_update (PermissionDb *self);
GBytes *       permission_db_get_content (PermissionDb *self);
const char *   permission_db_get_path (PermissionDb *self);
gboolean       permission_db_save_content (PermissionDb *self,
                                           GError   **error);
void           permission_db_save_content_async (PermissionDb          *self,
                                                 GCancellable       *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer            user_data);
gboolean       permission_db_save_content_finish (PermissionDb    *self,
                                                  GAsyncResult *res,
                                                  GError      **error);
void           permission_db_set_path (PermissionDb  *self,
                                       const char *path);


PermissionDbEntry  *permission_db_entry_ref (PermissionDbEntry *entry);
void            permission_db_entry_unref (PermissionDbEntry *entry);
GVariant *      permission_db_entry_get_data (PermissionDbEntry *entry);
const char **   permission_db_entry_list_apps (PermissionDbEntry *entry);
const char **   permission_db_entry_list_permissions (PermissionDbEntry *entry,
                                                      const char     *app_permissions_id);
GString *       permission_db_entry_print_string (PermissionDbEntry *entry,
                                                  GString        *string);

PermissionDbEntry  *permission_db_entry_new (GVariant *data);
PermissionDbEntry  *permission_db_entry_modify_data (PermissionDbEntry *entry,
                                                     GVariant       *data);
PermissionDbEntry  *permission_db_entry_set_app_permissions (PermissionDbEntry *entry,
                                                             const char     *app_permissions_id,
                                                             const char    **permissions);
PermissionDbEntry  *permission_db_entry_remove_app_permissions (PermissionDbEntry *entry,
                                                                const char        *app_permissions_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PermissionDb, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PermissionDbEntry, permission_db_entry_unref)

G_END_DECLS

#endif /* PERMISSION_DB_H */
