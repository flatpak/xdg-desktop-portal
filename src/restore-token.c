/*
 * Copyright 2021-2022 Endless OS Foundation, LLC
 * Copyright 2023 Red Hat, Inc
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
 */

#include "config.h"

#include "permissions.h"
#include "restore-token.h"

static GMutex transient_permissions_lock;
static GHashTable *transient_permissions;

#define RESTORE_DATA_TYPE "(suv)"

void
set_transient_permissions (const char *sender,
                           const char *restore_token,
                           GVariant *restore_data)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);

  if (!transient_permissions)
    {
      transient_permissions =
        g_hash_table_new_full (g_str_hash, g_str_equal,
                               g_free, (GDestroyNotify) g_variant_unref);
    }

  g_hash_table_insert (transient_permissions,
                       g_strdup_printf ("%s/%s", sender, restore_token),
                       g_variant_ref (restore_data));
}

void
delete_transient_permissions (const char *sender,
                              const char *restore_token)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);
  g_autofree char *id = NULL;

  if (!transient_permissions)
    return;

  id = g_strdup_printf ("%s/%s", sender, restore_token);
  g_hash_table_remove (transient_permissions, id);
}

GVariant *
get_transient_permissions (const char *sender,
                           const char *restore_token)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);
  g_autofree char *id = NULL;
  GVariant *permissions;

  if (!transient_permissions)
    return NULL;

  id = g_strdup_printf ("%s/%s", sender, restore_token);
  permissions = g_hash_table_lookup (transient_permissions, id);
  return permissions ? g_variant_ref (permissions) : NULL;
}

void
set_persistent_permissions (const char *table,
                            const char *app_id,
                            const char *restore_token,
                            GVariant *restore_data)
{
  g_autoptr(GError) error = NULL;

  set_permission_sync (app_id, table, restore_token, PERMISSION_YES);

  if (!xdp_dbus_impl_permission_store_call_set_value_sync (get_permission_store (),
                                                           table,
                                                           TRUE,
                                                           restore_token,
                                                           g_variant_new_variant (restore_data),
                                                           NULL,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error setting permission store value: %s", error->message);
    }
}

void
delete_persistent_permissions (const char *table,
                               const char *app_id,
                               const char *restore_token)
{

  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_permission_store_call_delete_sync (get_permission_store (),
                                                        table,
                                                        restore_token,
                                                        NULL,
                                                        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error deleting permission: %s", error->message);
    }
}

GVariant *
get_persistent_permissions (const char *table,
                            const char *app_id,
                            const char *restore_token)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GError) error = NULL;
  const char **permissions;

  if (!xdp_dbus_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                        table,
                                                        restore_token,
                                                        &perms,
                                                        &data,
                                                        NULL,
                                                        &error))
    {
      return NULL;
    }

  if (!perms || !g_variant_lookup (perms, app_id, "^a&s", &permissions))
    return NULL;

  if (!data)
    return NULL;

  return g_variant_get_child_value (data, 0);
}

void
remove_transient_permissions_for_sender (const char *sender)
{
  g_autoptr(GMutexLocker) locker = NULL;
  GHashTableIter iter;
  const char *key;

  locker = g_mutex_locker_new (&transient_permissions_lock);

  if (!transient_permissions)
    return;

  g_hash_table_iter_init (&iter, transient_permissions);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, NULL))
    {
      g_auto(GStrv) split = g_strsplit (key, "/", 2);

      if (split && split[0] && g_strcmp0 (split[0], sender) == 0)
        g_hash_table_iter_remove (&iter);
    }
}

void
replace_restore_token_with_data (Session *session,
                                 const char *table,
                                 GVariant **in_out_options,
                                 char **out_restore_token)
{
  GVariantIter options_iter;
  GVariantBuilder options_builder;
  char *key;
  GVariant *value;

  g_variant_iter_init (&options_iter, *in_out_options);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  while (g_variant_iter_next (&options_iter, "{sv}", &key, &value))
    {
      if (g_strcmp0 (key, "restore_token") == 0)
        {
          g_autofree char *restore_token = NULL;
          g_autoptr(GVariant) restore_data = NULL;

          restore_token = g_variant_dup_string (value, NULL);

          /* Lookup permissions in memory first, and fallback to the permission
           * store if not found. Immediately delete them now as a safety measure,
           * since they'll be stored again when the session is closed.
           *
           * Notice that transient mode uses the sender name, whereas persistent
           * mode uses the app id.
           */
          restore_data = get_transient_permissions (session->sender, restore_token);
          if (restore_data)
            {
              delete_transient_permissions (session->sender, restore_token);
            }
          else
            {
              restore_data = get_persistent_permissions (table,
                                                         session->app_id,
                                                         restore_token);
              if (restore_data)
                {
                  delete_persistent_permissions (table,
                                                 session->app_id,
                                                 restore_token);
                }
            }

          if (restore_data &&
              g_variant_check_format_string (restore_data, RESTORE_DATA_TYPE, FALSE))
            {
              g_debug ("Replacing 'restore_token' with portal-specific data");
              g_variant_builder_add (&options_builder, "{sv}",
                                     "restore_data", restore_data);
              *out_restore_token = g_steal_pointer (&restore_token);
            }
        }
      else
        {
          g_variant_builder_add (&options_builder, "{sv}",
                                 key, g_variant_ref (value));
        }

      g_free (key);
      g_variant_unref (value);
    }

  *in_out_options = g_variant_builder_end (&options_builder);
}

void
generate_and_save_restore_token (Session *session,
                                 const char *table,
                                 PersistMode persist_mode,
                                 char **in_out_restore_token,
                                 GVariant **in_out_restore_data)
{
  if (!*in_out_restore_data)
    {
      if (*in_out_restore_token)
        {
          delete_persistent_permissions (table,
                                         session->app_id,
                                         *in_out_restore_token);
          delete_transient_permissions (session->sender,
                                        *in_out_restore_token);
        }

      g_clear_pointer (in_out_restore_token, g_free);
      return;
    }

  switch (persist_mode)
    {
    case PERSIST_MODE_NONE:
      if (*in_out_restore_token)
        {
          delete_persistent_permissions (table,
                                         session->app_id,
                                         *in_out_restore_token);
          delete_transient_permissions (session->sender,
                                        *in_out_restore_token);
        }

      g_clear_pointer (in_out_restore_token, g_free);
      g_clear_pointer (in_out_restore_data, g_variant_unref);
      break;

    case PERSIST_MODE_TRANSIENT:
      if (!*in_out_restore_token)
        *in_out_restore_token = g_uuid_string_random ();

      set_transient_permissions (session->sender,
                                 *in_out_restore_token,
                                 *in_out_restore_data);
      break;

    case PERSIST_MODE_PERSISTENT:
      if (!*in_out_restore_token)
        *in_out_restore_token = g_uuid_string_random ();

      set_persistent_permissions (table,
                                  session->app_id,
                                  *in_out_restore_token,
                                  *in_out_restore_data);

      break;
    }
}

void
replace_restore_data_with_token (Session *session,
                                 const char *table,
                                 GVariant **in_out_results,
                                 PersistMode *in_out_persist_mode,
                                 char **in_out_restore_token,
                                 GVariant **in_out_restore_data)
{
  g_autoptr(GVariant) results = *in_out_results;
  GVariantBuilder results_builder;
  GVariantIter iter;
  const char *key;
  GVariant *value;
  gboolean found_restore_data = FALSE;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  g_variant_iter_init (&iter, results);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      if (g_strcmp0 (key, "restore_data") == 0)
        {
          if (g_variant_check_format_string (value, RESTORE_DATA_TYPE, FALSE))
            {
              *in_out_restore_data = g_variant_ref_sink (value);
              found_restore_data = TRUE;
            }
          else
            {
              g_warning ("Received restore data in invalid variant format ('%s'; expected '%s')",
                         g_variant_get_type_string (value),
                         RESTORE_DATA_TYPE);
            }
        }
      else if (g_strcmp0 (key, "persist_mode") == 0)
        {
          *in_out_persist_mode = MIN (*in_out_persist_mode,
                                      g_variant_get_uint32 (value));
        }
      else
        {
          g_variant_builder_add (&results_builder, "{sv}", key, value);
        }
    }

  if (found_restore_data)
    {
      g_debug ("Replacing restore data received from portal impl with a token");

      generate_and_save_restore_token (session,
                                       table,
                                       *in_out_persist_mode,
                                       in_out_restore_token,
                                       in_out_restore_data);
      g_variant_builder_add (&results_builder, "{sv}", "restore_token",
                             g_variant_new_string (*in_out_restore_token));
    }
  else
    {
      *in_out_persist_mode = PERSIST_MODE_NONE;
    }

  *in_out_results = g_variant_builder_end (&results_builder);
}
