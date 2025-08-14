/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include "permission-store-dbus.h"
#include "xdg-permission-store.h"
#include "permission-db.h"
#include "src/xdp-utils.h"

GHashTable *tables = NULL;

typedef struct
{
  char      *name;
  PermissionDb *db;
  GList     *outstanding_writes;
  GList     *current_writes;
  gboolean   writing;
} Table;

static void start_writeout (Table *table);

static void
table_free (Table *table)
{
  g_free (table->name);
  g_object_unref (table->db);
  g_free (table);
}

static Table *
lookup_table (const char            *name,
              GDBusMethodInvocation *invocation)
{
  Table *table;
  PermissionDb *db;
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;

  g_autoptr(GError) error = NULL;

  table = g_hash_table_lookup (tables, name);
  if (table != NULL)
    return table;

  dir = g_build_filename (g_get_user_data_dir (), "flatpak/db", NULL);
  g_mkdir_with_parents (dir, 0755);

  path = g_build_filename (dir, name, NULL);
  db = permission_db_new (path, FALSE, &error);
  if (db == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Unable to load db file %s: %s", name, error->message);
      return NULL;
    }

  table = g_new0 (Table, 1);
  table->name = g_strdup (name);
  table->db = db;

  g_hash_table_insert (tables, table->name, table);

  return table;
}

static void
writeout_done (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  Table *table = user_data;
  GList *l;

  g_autoptr(GError) error = NULL;
  gboolean ok;

  ok = permission_db_save_content_finish (table->db, res, &error);

  for (l = table->current_writes; l != NULL; l = l->next)
    {
      GDBusMethodInvocation *invocation = l->data;

      if (ok)
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("()"));
      else
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                               "Unable to write db: %s", error->message);
    }

  g_list_free (table->current_writes);
  table->current_writes = NULL;
  table->writing = FALSE;

  if (table->outstanding_writes != NULL)
    start_writeout (table);
}

static void
start_writeout (Table *table)
{
  g_assert (table->current_writes == NULL);
  table->current_writes = table->outstanding_writes;
  table->outstanding_writes = NULL;
  table->writing = TRUE;

  permission_db_update (table->db);

  permission_db_save_content_async (table->db, NULL, writeout_done, table);
}

static void
ensure_writeout (Table                 *table,
                 GDBusMethodInvocation *invocation)
{
  table->outstanding_writes = g_list_prepend (table->outstanding_writes, invocation);

  if (!table->writing)
    start_writeout (table);
}

static gboolean
handle_list (XdgPermissionStore     *object,
             GDBusMethodInvocation  *invocation,
             const gchar            *table_name)
{
  Table *table;

  g_auto(GStrv) ids = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  ids = permission_db_list_ids (table->db);

  xdg_permission_store_complete_list (object, invocation, (const char * const *) ids);

  return TRUE;
}

static GVariant *
get_app_permissions (PermissionDbEntry *entry)
{
  g_autofree const char **apps_permissions_ids = NULL;
  GVariantBuilder builder;
  int i;

  apps_permissions_ids = permission_db_entry_list_apps (entry);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));

  for (i = 0; apps_permissions_ids[i] != NULL; i++)
    {
      g_autofree const char **permissions =
        permission_db_entry_list_permissions (entry, apps_permissions_ids[i]);
      g_variant_builder_add_value (&builder,
                                   g_variant_new ("{s@as}",
                                                  apps_permissions_ids[i],
                                                  g_variant_new_strv (permissions, -1)));
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
handle_lookup (XdgPermissionStore     *object,
               GDBusMethodInvocation  *invocation,
               const gchar            *table_name,
               const gchar            *id)
{
  Table *table;

  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) permissions = NULL;
  g_autoptr(PermissionDbEntry) entry = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  entry = permission_db_lookup (table->db, id);
  if (entry == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "No entry for %s", id);
      return TRUE;
    }

  data = permission_db_entry_get_data (entry);
  permissions = get_app_permissions (entry);

  xdg_permission_store_complete_lookup (object, invocation,
                                        permissions,
                                        g_variant_new_variant (data));

  return TRUE;
}

static void
emit_deleted (XdgPermissionStore     *object,
              const gchar            *table_name,
              const gchar            *id,
              PermissionDbEntry         *entry)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) permissions = NULL;

  data = permission_db_entry_get_data (entry);
  permissions = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sas}"), NULL, 0));

  xdg_permission_store_emit_changed (object,
                                     table_name, id,
                                     TRUE,
                                     g_variant_new_variant (data),
                                     permissions);
}


static void
emit_changed (XdgPermissionStore     *object,
              const gchar            *table_name,
              const gchar            *id,
              PermissionDbEntry         *entry)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) permissions = NULL;

  data = permission_db_entry_get_data (entry);
  permissions = get_app_permissions (entry);

  xdg_permission_store_emit_changed (object,
                                     table_name, id,
                                     FALSE,
                                     g_variant_new_variant (data),
                                     permissions);
}

static gboolean
handle_delete (XdgPermissionStore     *object,
               GDBusMethodInvocation  *invocation,
               const gchar            *table_name,
               const gchar            *id)
{
  Table *table;

  g_autoptr(PermissionDbEntry) entry = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  entry = permission_db_lookup (table->db, id);
  if (entry == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "No entry for %s", id);
      return TRUE;
    }

  permission_db_set_entry (table->db, id, NULL);
  emit_deleted (object, table_name, id, entry);

  ensure_writeout (table, invocation);

  return TRUE;
}

static gboolean
handle_delete_permission (XdgPermissionStore     *object,
                          GDBusMethodInvocation  *invocation,
                          const char             *table_name,
                          const char             *id,
                          const char             *app)
{
  Table *table;
  const char *permissions_id = NULL;
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autoptr(PermissionDbEntry) new_entry = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  entry = permission_db_lookup (table->db, id);
  if (entry == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "No entry for %s", id);
      return TRUE;
    }

  permissions_id = app;
  new_entry = permission_db_entry_remove_app_permissions (entry, permissions_id);
  permission_db_set_entry (table->db, id, new_entry);
  emit_changed (object, table_name, id, new_entry);

  ensure_writeout (table, invocation);

  return TRUE;
}

static gboolean
handle_get_permission (XdgPermissionStore     *object,
                       GDBusMethodInvocation  *invocation,
                       const char             *table_name,
                       const char             *id,
                       const char             *app)
{
  Table *table;

  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autofree const char **permission = NULL;
  const char *permissions_id;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  entry = permission_db_lookup (table->db, id);
  if (entry == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "No entry for %s", id);
      return TRUE;
    }

  permissions_id = app;
  permission = permission_db_entry_list_permissions (entry, permissions_id);

  xdg_permission_store_complete_get_permission (object, invocation, permission);

  return TRUE;
}

static gboolean
handle_set (XdgPermissionStore     *object,
            GDBusMethodInvocation  *invocation,
            const gchar            *table_name,
            gboolean                create,
            const gchar            *id,
            GVariant               *app_permissions,
            GVariant               *data)
{
  Table *table;
  GVariantIter iter;
  GVariant *child;

  g_autoptr(GVariant) data_child = NULL;
  g_autoptr(PermissionDbEntry) old_entry = NULL;
  g_autoptr(PermissionDbEntry) new_entry = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  old_entry = permission_db_lookup (table->db, id);
  if (old_entry == NULL && !create)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Id %s not found", id);
      return TRUE;
    }

  data_child = g_variant_get_child_value (data, 0);
  new_entry = permission_db_entry_new (data_child);

  /* Add all the given app permissions */

  g_variant_iter_init (&iter, app_permissions);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      g_autoptr(PermissionDbEntry) old_entry = NULL;
      const char *child_app_id;
      const char *child_permissions_id;
      g_autofree const char **permissions;

      g_variant_get (child, "{&s^a&s}", &child_app_id, &permissions);

      old_entry = new_entry;
      child_permissions_id = child_app_id;
      new_entry = permission_db_entry_set_app_permissions (new_entry,
                                                           child_permissions_id,
                                                           (const char **) permissions);

      g_variant_unref (child);
    }

  permission_db_set_entry (table->db, id, new_entry);
  emit_changed (object, table_name, id, new_entry);

  ensure_writeout (table, invocation);

  return TRUE;
}

static gboolean
handle_set_permission (XdgPermissionStore     *object,
                       GDBusMethodInvocation  *invocation,
                       const gchar            *table_name,
                       gboolean                create,
                       const gchar            *id,
                       const gchar            *app,
                       const gchar *const     *permissions)
{
  Table *table;
  const char *permissions_id;
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autoptr(PermissionDbEntry) new_entry = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  entry = permission_db_lookup (table->db, id);
  if (entry == NULL)
    {
      if (create)
        {
          entry = permission_db_entry_new (NULL);
        }
      else
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 "Id %s not found", id);
          return TRUE;
        }
    }

  permissions_id = app;
  new_entry = permission_db_entry_set_app_permissions (entry, permissions_id,
                                                       (const char **) permissions);
  permission_db_set_entry (table->db, id, new_entry);
  emit_changed (object, table_name, id, new_entry);

  ensure_writeout (table, invocation);

  return TRUE;
}

static gboolean
handle_set_value (XdgPermissionStore     *object,
                  GDBusMethodInvocation  *invocation,
                  const gchar            *table_name,
                  gboolean                create,
                  const gchar            *id,
                  GVariant               *data)
{
  Table *table;

  g_autoptr(GVariant) data_child = NULL;
  g_autoptr(PermissionDbEntry) entry = NULL;
  g_autoptr(PermissionDbEntry) new_entry = NULL;

  table = lookup_table (table_name, invocation);
  if (table == NULL)
    return TRUE;

  data_child = g_variant_get_child_value (data, 0);

  entry = permission_db_lookup (table->db, id);
  if (entry == NULL)
    {
      if (create)
        {
          new_entry = permission_db_entry_new (data_child);
        }
      else
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 "Id %s not found", id);
          return TRUE;
        }
    }
  else
    {
      new_entry = permission_db_entry_modify_data (entry, data_child);
    }

  permission_db_set_entry (table->db, id, new_entry);
  emit_changed (object, table_name, id, new_entry);

  ensure_writeout (table, invocation);

  return TRUE;
}

void
xdg_permission_store_start (GDBusConnection *connection)
{
  XdgPermissionStore *store;
  GError *error = NULL;

  g_debug ("Starting permission store");

  tables = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  g_free, (GDestroyNotify) table_free);

  store = xdg_permission_store_skeleton_new ();

  xdg_permission_store_set_version (XDG_PERMISSION_STORE (store), 2);

  g_signal_connect (store, "handle-list", G_CALLBACK (handle_list), NULL);
  g_signal_connect (store, "handle-lookup", G_CALLBACK (handle_lookup), NULL);
  g_signal_connect (store, "handle-set", G_CALLBACK (handle_set), NULL);
  g_signal_connect (store, "handle-set-permission", G_CALLBACK (handle_set_permission), NULL);
  g_signal_connect (store, "handle-set-value", G_CALLBACK (handle_set_value), NULL);
  g_signal_connect (store, "handle-delete", G_CALLBACK (handle_delete), NULL);
  g_signal_connect (store, "handle-delete-permission", G_CALLBACK (handle_delete_permission), NULL);
  g_signal_connect (store, "handle-get-permission", G_CALLBACK (handle_get_permission), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (store),
                                         connection,
                                         "/org/freedesktop/impl/portal/PermissionStore",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }
}
