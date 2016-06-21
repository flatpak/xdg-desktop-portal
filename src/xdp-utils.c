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

#include "xdp-utils.h"
#include "flatpak-portal-error.h"

G_LOCK_DEFINE (app_ids);
static GHashTable *app_ids;

static void
ensure_app_ids (void)
{
  if (app_ids == NULL)
    app_ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     g_free, g_free);
}

static char *
get_app_id_from_pid (pid_t pid,
                     GError **error)
{
  g_autofree char *path = NULL;
  g_autofree char *content = NULL;
  g_auto(GStrv) lines = NULL;
  int i;

  path = g_strdup_printf ("/proc/%u/cgroup", pid);
  if (!g_file_get_contents (path, &content, NULL, error))
    {
      g_prefix_error (error, "Can't find peer app id: ");
      return NULL;
    }

  lines =  g_strsplit (content, "\n", -1);
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
                  return g_strdup (name);
                }
            }
          else
            {
              return g_strdup ("");
            }
        }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Can't find peer app id: No name=systemd cgroup");
  return NULL;
}

static char *
xdp_connection_lookup_app_id_sync (GDBusConnection       *connection,
                                   const char            *sender,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  g_autoptr(GDBusMessage) msg = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  char *app_id = NULL;
  GVariant *body;
  guint32 pid;

  G_LOCK (app_ids);
  if (app_ids)
    app_id = g_strdup (g_hash_table_lookup (app_ids, sender));
  G_UNLOCK (app_ids);

  if (app_id != NULL)
    return app_id;

  msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "GetConnectionUnixProcessID");
  g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

  reply = g_dbus_connection_send_message_with_reply_sync (connection, msg,
                                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                          30000,
                                                          NULL,
                                                          cancellable,
                                                          error);
  if (reply == NULL)
    return NULL;

  if (g_dbus_message_get_message_type (reply) == G_DBUS_MESSAGE_TYPE_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer app id");
      return NULL;
    }

  body = g_dbus_message_get_body (reply);

  g_variant_get (body, "(u)", &pid);

  app_id = get_app_id_from_pid (pid, error);
  if (app_id)
    {
      G_LOCK (app_ids);
      ensure_app_ids ();
      g_hash_table_insert (app_ids, g_strdup (sender), g_strdup (app_id));
      G_UNLOCK (app_ids);
    }

  return app_id;
}

char *
xdp_invocation_lookup_app_id_sync (GDBusMethodInvocation *invocation,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  return xdp_connection_lookup_app_id_sync (connection, sender, cancellable, error);
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

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      G_LOCK (app_ids);
      if (app_ids)
        g_hash_table_remove (app_ids, name);
      G_UNLOCK (app_ids);
    }
}

void
xdp_connection_track_name_owners (GDBusConnection *connection)
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
