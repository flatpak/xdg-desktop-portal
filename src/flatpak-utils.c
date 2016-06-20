/*
 * Copyright © 2014 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>

#include "flatpak-utils.h"
#include "flatpak-portal-error.h"

static GHashTable *app_ids;

typedef struct
{
  char    *name;
  char    *app_id;
  gboolean exited;
  GList   *pending;
} AppIdInfo;

static void
app_id_info_free (AppIdInfo *info)
{
  g_free (info->name);
  g_free (info->app_id);
  g_free (info);
}

static void
ensure_app_ids (void)
{
  if (app_ids == NULL)
    app_ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     NULL, (GDestroyNotify) app_id_info_free);
}

static void
got_credentials_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  AppIdInfo *info = user_data;

  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(GError) error = NULL;
  GList *l;

  reply = g_dbus_connection_send_message_with_reply_finish (G_DBUS_CONNECTION (source_object),
                                                            res, &error);

  if (!info->exited &&
      reply != NULL &&
      g_dbus_message_get_message_type (reply) != G_DBUS_MESSAGE_TYPE_ERROR)
    {
      GVariant *body = g_dbus_message_get_body (reply);
      guint32 pid;
      g_autofree char *path = NULL;
      g_autofree char *content = NULL;

      g_variant_get (body, "(u)", &pid);

      path = g_strdup_printf ("/proc/%u/cgroup", pid);

      if (g_file_get_contents (path, &content, NULL, NULL))
        {
          gchar **lines =  g_strsplit (content, "\n", -1);
          int i;

          for (i = 0; lines[i] != NULL; i++)
            {
              if (g_str_has_prefix (lines[i], "1:name=systemd:"))
                {
                  const char *unit = lines[i] + strlen ("1:name=systemd:");
                  g_autofree char *scope = g_path_get_basename (unit);

                  if (g_str_has_prefix (scope, "flatpak-") &&
                      g_str_has_suffix (scope, ".scope"))
                    {
                      const char *name = scope + strlen ("flatpak-");
                      char *dash = strchr (name, '-');
                      if (dash != NULL)
                        {
                          *dash = 0;
                          info->app_id = g_strdup (name);
                        }
                    }
                  else
                    {
                      info->app_id = g_strdup ("");
                    }
                }
            }
          g_strfreev (lines);
        }
    }

  for (l = info->pending; l != NULL; l = l->next)
    {
      GTask *task = l->data;

      if (info->app_id == NULL)
        g_task_return_new_error (task, FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_FAILED,
                                 "Can't find app id");
      else
        g_task_return_pointer (task, g_strdup (info->app_id), g_free);
    }

  g_list_free_full (info->pending, g_object_unref);
  info->pending = NULL;

  if (info->app_id == NULL)
    g_hash_table_remove (app_ids, info->name);
}

typedef struct {
  GObject *source;
  GAsyncReadyCallback callback;
  gpointer user_data;
} AppIdData;

static void
lookup_callback (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
  AppIdData *data = user_data;

  data->callback (data->source, result, data->user_data);
  g_object_unref (data->source);
  g_free (data);
}

void
flatpak_invocation_lookup_app_id (GDBusMethodInvocation *invocation,
                                  GCancellable          *cancellable,
                                  GAsyncReadyCallback    callback,
                                  gpointer               user_data)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  AppIdData *data;

  data = g_new (AppIdData, 1);
  data->source = g_object_ref (invocation);
  data->callback = callback;
  data->user_data = user_data;

  flatpak_connection_lookup_app_id (connection, sender, cancellable, lookup_callback, data);
}

void
flatpak_connection_lookup_app_id (GDBusConnection       *connection,
                                  const char            *sender,
                                  GCancellable          *cancellable,
                                  GAsyncReadyCallback    callback,
                                  gpointer               user_data)
{
  g_autoptr(GTask) task = NULL;
  AppIdInfo *info;

  task = g_task_new (connection, cancellable, callback, user_data);

  ensure_app_ids ();

  info = g_hash_table_lookup (app_ids, sender);

  if (info == NULL)
    {
      info = g_new0 (AppIdInfo, 1);
      info->name = g_strdup (sender);
      g_hash_table_insert (app_ids, info->name, info);
    }

  if (info->app_id)
    {
      g_task_return_pointer (task, g_strdup (info->app_id), g_free);
    }
  else
    {
      if (info->pending == NULL)
        {
          g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
                                                                        "/org/freedesktop/DBus",
                                                                        "org.freedesktop.DBus",
                                                                        "GetConnectionUnixProcessID");
          g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

          g_dbus_connection_send_message_with_reply (connection, msg,
                                                     G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                     30000,
                                                     NULL,
                                                     cancellable,
                                                     got_credentials_cb,
                                                     info);
        }

      info->pending = g_list_prepend (info->pending, g_object_ref (task));
    }
}

char *
flatpak_invocation_lookup_app_id_finish (GDBusMethodInvocation *invocation,
                                         GAsyncResult          *result,
                                         GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);

  return flatpak_connection_lookup_app_id_finish (connection, result, error);
}


char *
flatpak_connection_lookup_app_id_finish (GDBusConnection       *connection,
                                         GAsyncResult          *result,
                                         GError               **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}


static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(sss)", &name, &from, &to);

  ensure_app_ids ();

g_print ("name owner changed for: %s from %s to %s\n", name, from, to);
  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      AppIdInfo *info = g_hash_table_lookup (app_ids, name);

      if (info != NULL)
        {
          info->exited = TRUE;
g_print ("set info->exited\n");
          if (info->pending == NULL)
            g_hash_table_remove (app_ids, name);
        }
    }
}

void
flatpak_connection_track_name_owners (GDBusConnection *connection)
{
  g_dbus_connection_signal_subscribe (connection,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);
}
