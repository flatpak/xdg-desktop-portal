/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-session-persistence.h"

#include "xdp-permissions.h"
#include "xdp-utils.h"

static GMutex transient_permissions_lock;
static GHashTable *transient_permissions;

#define RESTORE_DATA_TYPE "(suv)"

static void
on_app_info_disconnected (XdpAppInfo *app_info,
                          gpointer    user_data)
{
  const char *peer = xdp_app_info_get_sender (app_info);
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

      if (split && split[0] && g_strcmp0 (split[0], peer) == 0)
        g_hash_table_iter_remove (&iter);
    }
}

/* Connected once per app info, so that its tokens go when the peer does */
static void
watch_app_info (XdpAppInfo *app_info)
{
  static GQuark quark_watched;

  if (quark_watched == 0)
    quark_watched = g_quark_from_static_string ("xdp-persistence-watched");

  if (g_object_get_qdata (G_OBJECT (app_info), quark_watched))
    return;

  g_object_set_qdata (G_OBJECT (app_info), quark_watched, GINT_TO_POINTER (TRUE));

  g_signal_connect (app_info, "disconnected",
                    G_CALLBACK (on_app_info_disconnected),
                    NULL);
}

/* Scoped by peer key: the hash key is "peer/token", so a token is only valid
 * for the peer that created it */
void
xdp_session_persistence_set_transient_permissions (XdpAppInfo *app_info,
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

  watch_app_info (app_info);

  g_hash_table_insert (transient_permissions,
                       g_strdup_printf ("%s/%s", xdp_app_info_get_sender (app_info), restore_token),
                       g_variant_ref (restore_data));
}

void
xdp_session_persistence_delete_transient_permissions (XdpAppInfo *app_info,
                                                      const char *restore_token)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);
  g_autofree char *id = NULL;

  if (!transient_permissions)
    return;

  id = g_strdup_printf ("%s/%s", xdp_app_info_get_sender (app_info), restore_token);
  g_hash_table_remove (transient_permissions, id);
}

GVariant *
xdp_session_persistence_get_transient_permissions (XdpAppInfo *app_info,
                                                   const char *restore_token)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);
  g_autofree char *id = NULL;
  GVariant *permissions;

  if (!transient_permissions)
    return NULL;

  id = g_strdup_printf ("%s/%s", xdp_app_info_get_sender (app_info), restore_token);
  permissions = g_hash_table_lookup (transient_permissions, id);
  return permissions ? g_variant_ref (permissions) : NULL;
}

/* Scoped by app id: the store entry is keyed by token, but lookup checks that
 * the app id has access */
void
xdp_session_persistence_set_persistent_permissions (XdpAppInfo *app_info,
                                                    const char *table,
                                                    const char *restore_token,
                                                    GVariant *restore_data)
{
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) permissions_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sas}"));
  g_auto(GStrv) permission = NULL;

  permission = xdp_permissions_from_tristate (XDP_PERMISSION_YES);

  g_variant_builder_add (&permissions_builder, "{s^a&s}", xdp_app_info_get_id (app_info), permission);

  if (!xdp_dbus_impl_permission_store_call_set_sync (xdp_get_permission_store (),
                                                     table,
                                                     TRUE,
                                                     restore_token,
                                                     g_variant_builder_end (&permissions_builder),
                                                     g_variant_new_variant (restore_data),
                                                     NULL,
                                                     &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error setting permission store value: %s", error->message);
    }
}

void
xdp_session_persistence_delete_persistent_permissions (XdpAppInfo *app_info,
                                                       const char *table,
                                                       const char *restore_token)
{

  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_permission_store_call_delete_sync (xdp_get_permission_store (),
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
xdp_session_persistence_get_persistent_permissions (XdpAppInfo *app_info,
                                                    const char *table,
                                                    const char *restore_token)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree const char **permissions = NULL;

  if (!xdp_dbus_impl_permission_store_call_lookup_sync (xdp_get_permission_store (),
                                                        table,
                                                        restore_token,
                                                        &perms,
                                                        &data,
                                                        NULL,
                                                        &error))
    {
      return NULL;
    }

  if (!perms || !g_variant_lookup (perms, xdp_app_info_get_id (app_info), "^a&s", &permissions))
    return NULL;

  if (!data)
    return NULL;

  return g_variant_get_child_value (data, 0);
}

void
xdp_session_persistence_replace_restore_token_with_data (XdpAppInfo *app_info,
                                                         const char *table,
                                                         GVariant **in_out_options,
                                                         char **out_restore_token)
{
  GVariantIter options_iter;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  char *key;
  GVariant *value;

  g_variant_iter_init (&options_iter, *in_out_options);

  while (g_variant_iter_next (&options_iter, "{&sv}", &key, &value))
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
          restore_data =
            xdp_session_persistence_get_transient_permissions (app_info,
                                                               restore_token);
          if (restore_data)
            {
              xdp_session_persistence_delete_transient_permissions (app_info,
                                                                    restore_token);
            }
          else
            {
              restore_data =
                xdp_session_persistence_get_persistent_permissions (app_info,
                                                                    table,
                                                                    restore_token);
              if (restore_data)
                {
                  xdp_session_persistence_delete_persistent_permissions (app_info,
                                                                         table,
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
                                 key, value);
        }

      g_clear_pointer (&value, g_variant_unref);
    }

  g_clear_pointer (in_out_options, g_variant_unref);
  *in_out_options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
}

void
xdp_session_persistence_generate_and_save_restore_token (XdpAppInfo *app_info,
                                                         const char *table,
                                                         XdpSessionPersistenceMode persist_mode,
                                                         char **in_out_restore_token,
                                                         GVariant **in_out_restore_data)
{
  if (!*in_out_restore_data)
    {
      if (*in_out_restore_token)
        {
          xdp_session_persistence_delete_persistent_permissions (app_info,
                                                                 table,
                                                                 *in_out_restore_token);
          xdp_session_persistence_delete_transient_permissions (app_info,
                                                                *in_out_restore_token);
        }

      g_clear_pointer (in_out_restore_token, g_free);
      return;
    }

  switch (persist_mode)
    {
    case XDP_SESSION_PERSISTENCE_MODE_NONE:
      if (*in_out_restore_token)
        {
          xdp_session_persistence_delete_persistent_permissions (app_info,
                                                                 table,
                                                                 *in_out_restore_token);
          xdp_session_persistence_delete_transient_permissions (app_info,
                                                                *in_out_restore_token);
        }

      g_clear_pointer (in_out_restore_token, g_free);
      g_clear_pointer (in_out_restore_data, g_variant_unref);
      break;

    case XDP_SESSION_PERSISTENCE_MODE_TRANSIENT:
      if (!*in_out_restore_token)
        *in_out_restore_token = xdp_generate_token ();

      xdp_session_persistence_set_transient_permissions (app_info,
                                                         *in_out_restore_token,
                                                         *in_out_restore_data);
      break;

    case XDP_SESSION_PERSISTENCE_MODE_PERSISTENT:
      if (!*in_out_restore_token)
        *in_out_restore_token = xdp_generate_token ();

      xdp_session_persistence_set_persistent_permissions (app_info,
                                                          table,
                                                          *in_out_restore_token,
                                                          *in_out_restore_data);

      break;
    }
}

void
xdp_session_persistence_replace_restore_data_with_token (XdpAppInfo *app_info,
                                                         const char *table,
                                                         GVariant **in_out_results,
                                                         XdpSessionPersistenceMode *in_out_persist_mode,
                                                         char **in_out_restore_token,
                                                         GVariant **in_out_restore_data)
{
  g_autoptr(GVariant) results = *in_out_results;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  GVariantIter iter;
  const char *key;
  GVariant *value;
  gboolean found_restore_data = FALSE;

  g_variant_iter_init (&iter, results);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      if (g_strcmp0 (key, "restore_data") == 0)
        {
          if (g_variant_check_format_string (value, RESTORE_DATA_TYPE, FALSE))
            {
              *in_out_restore_data = g_variant_ref (value);
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
      g_clear_pointer (&value, g_variant_unref);
    }

  if (found_restore_data)
    {
      g_debug ("Replacing restore data received from portal impl with a token");

      xdp_session_persistence_generate_and_save_restore_token (app_info,
                                                               table,
                                                               *in_out_persist_mode,
                                                               in_out_restore_token,
                                                               in_out_restore_data);
      if (*in_out_restore_token)
        {
          g_variant_builder_add (&results_builder, "{sv}", "restore_token",
                                 g_variant_new_string (*in_out_restore_token));
        }
    }
  else
    {
      *in_out_persist_mode = XDP_SESSION_PERSISTENCE_MODE_NONE;
      g_clear_pointer (in_out_restore_token, g_free);
    }

  *in_out_results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
}


gboolean
xdp_session_persistence_validate_restore_token (const char  *restore_token,
                                                GError     **error)
{
  /* Accept both xdp_generate_token() format and legacy UUID strings
   * from older portal versions */
  if (!xdp_is_valid_token (restore_token) &&
      !g_uuid_string_is_valid (restore_token))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Restore token is not valid");
      return FALSE;
    }

  return TRUE;
}

gboolean
xdp_session_persistence_validate_persist_mode (XdpSessionPersistenceMode   mode,
                                               GError                    **error)
{
  if (mode > XDP_SESSION_PERSISTENCE_MODE_PERSISTENT)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid persist mode %x", mode);
      return FALSE;
    }

  return TRUE;
}
