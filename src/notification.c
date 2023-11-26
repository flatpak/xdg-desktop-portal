/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <gio/gio.h>
#include <gio/gunixoutputstream.h>

#include "notification.h"
#include "call.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "notifications"
#define PERMISSION_ID "notification"

typedef struct _Notification Notification;
typedef struct _NotificationClass NotificationClass;

struct _Notification
{
  XdpDbusNotificationSkeleton parent_instance;
};

struct _NotificationClass
{
  XdpDbusNotificationSkeletonClass parent_class;
};

static XdpDbusImplNotification *impl;
static Notification *notification;
G_LOCK_DEFINE (active);
static GHashTable *active;

typedef struct {
  char *app_id;
  char *id;
} Pair;

static guint
pair_hash (gconstpointer v)
{
  const Pair *p = v;

  return g_str_hash (p->app_id) + g_str_hash (p->id);
}

static gboolean
pair_equal (gconstpointer v1,
            gconstpointer v2)
{
  const Pair *p1 = v1;
  const Pair *p2 = v2;

  return g_str_equal (p1->app_id, p2->app_id) && g_str_equal (p1->id, p2->id);
}

static void
pair_free (gpointer v)
{
  Pair *p = v;

  g_free (p->app_id);
  g_free (p->id);
  g_free (p);
}

static Pair *
pair_copy (Pair *o)
{
  Pair *p;

  p = g_new (Pair, 1);
  p->app_id = g_strdup (o->app_id);
  p->id = g_strdup (o->id);

  return p;
}

struct _CallData {
  GObject parent_instance;
  GDBusMethodInvocation *inv;
  XdpAppInfo *app_info;
  GMutex mutex;

  char *sender;
  char *id;
  GVariant *notification;
};

G_DECLARE_FINAL_TYPE (CallData, call_data, CALL, DATA, GObject)
G_DEFINE_TYPE (CallData, call_data, G_TYPE_OBJECT);
#define CALL_DATA_AUTOLOCK(call_data) G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) GMutex * G_PASTE (request_auto_unlock, __LINE__) = auto_lock_helper (&call_data->mutex);

static void
call_data_init (CallData *call_data)
{
  g_mutex_init (&call_data->mutex);
}

static CallData *
call_data_new (GDBusMethodInvocation *inv,
               XdpAppInfo            *app_info,
               const char            *sender,
               const char            *id,
               GVariant              *notification)
{
  CallData *call_data = g_object_new (call_data_get_type(),  NULL);

  call_data->inv = g_object_ref (inv);
  call_data->app_info = xdp_app_info_ref (app_info);
  call_data->sender = g_strdup (sender);
  call_data->id = g_strdup (id);
  if (notification)
    call_data->notification = g_variant_ref (notification);

  return call_data;
}

static void
call_data_finalize (GObject *object)
{
  CallData *call_data = CALL_DATA (object);

  g_object_unref (call_data->inv);
  xdp_app_info_unref (call_data->app_info);
  g_free (call_data->id);
  g_free (call_data->sender);
  if (call_data->notification)
    g_variant_unref (call_data->notification);

  G_OBJECT_CLASS (call_data_parent_class)->finalize (object);
}

static void
call_data_class_init (CallDataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = call_data_finalize;
}

GType notification_get_type (void) G_GNUC_CONST;
static void notification_iface_init (XdpDbusNotificationIface *iface);

G_DEFINE_TYPE_WITH_CODE (Notification, notification,
                         XDP_DBUS_TYPE_NOTIFICATION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_NOTIFICATION,
                                                notification_iface_init));

static void
add_done (GObject *source,
          GAsyncResult *result,
          gpointer data)
{
  g_autoptr(CallData) call_data = data;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_notification_call_add_notification_finish (impl, result, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }
  else
    {
      Pair p;

      p.app_id = (char *)xdp_app_info_get_id (call_data->app_info);
      p.id = call_data->id;

      G_LOCK (active);
      g_hash_table_insert (active, pair_copy (&p), g_strdup (call_data->sender));
      G_UNLOCK (active);
    }
}

static gboolean
get_notification_allowed (const char *app_id)
{
  Permission permission;

  permission = get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);

  if (permission == PERMISSION_NO)
    return FALSE;

  if (permission == PERMISSION_UNSET)
    {
      g_debug ("No notification permissions stored for %s: allowing", app_id);

      set_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID, PERMISSION_YES);
    }

  return TRUE;
}

static gboolean
check_value_type (const char *key,
                  GVariant *value,
                  const GVariantType *type,
                  GError **error)
{
  if (g_variant_is_of_type (value, type))
    return TRUE;

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "expected type for key %s is %s, found %s",
               key, (const char *)type, (const char *)g_variant_get_type (value));

  return FALSE;
}

static gboolean
check_priority (GVariant *value,
                GError **error)
{
  const char *priorities[] = { "low", "normal", "high", "urgent", NULL };

  if (!check_value_type ("priority", value, G_VARIANT_TYPE_STRING, error))
    return FALSE;

  if (!g_strv_contains (priorities, g_variant_get_string (value, NULL)))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "%s not a priority", g_variant_get_string (value, NULL));
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_button (GVariant *button,
              GError **error)
{
  int i;
  gboolean has_label = FALSE;
  gboolean has_action = FALSE;

  for (i = 0; i < g_variant_n_children (button); i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (button, i, "{&sv}", &key, &value);
      if (strcmp (key, "label") == 0)
        has_label = TRUE;
      else if (strcmp (key, "action") == 0)
        has_action = TRUE;
      else if (strcmp (key, "target") == 0)
        ;
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "%s not valid key", key);
          return FALSE;
        }
    }

  if (!has_label)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "label key is missing");
      return FALSE;
    }

  if (!has_action)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "action key is missing");
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_buttons (GVariant *value,
               GError **error)
{
  int i;

  if (!check_value_type ("buttons", value, G_VARIANT_TYPE ("aa{sv}"), error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (value); i++)
    {
      g_autoptr(GVariant) button = g_variant_get_child_value (value, i);

      if (!check_button (button, error))
        {
          g_prefix_error (error, "invalid button: ");
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
check_serialized_icon (GVariant *value,
                       GError **error)
{
  g_autoptr(GIcon) icon = g_icon_deserialize (value);

  if (!icon)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "invalid icon");
      return FALSE;
    }

  if (!G_IS_BYTES_ICON (icon) && !G_IS_THEMED_ICON (icon))
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "invalid icon");
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_notification (GVariant *notification,
                    GError **error)
{
  int i;

  if (!check_value_type ("notification", notification, G_VARIANT_TYPE_VARDICT, error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (notification); i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (notification, i, "{&sv}", &key, &value);
      if (strcmp (key, "title") == 0 ||
          strcmp (key, "body") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;
        }
      else if (strcmp (key, "icon") == 0)
        {
          if (!check_serialized_icon (value, error))
            return FALSE;
        }
      else if (strcmp (key, "priority") == 0)
        {
          if (!check_priority (value, error))
            return FALSE;
        }
      else if (strcmp (key, "default-action") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;
        }
      else if (strcmp (key, "default-action-target") == 0)
        ;
      else if (strcmp (key, "buttons") == 0)
        {
          if (!check_buttons (value, error))
            return FALSE;
        }
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "%s not valid key", key);
          return FALSE;
        }
    }

  return TRUE;
}

static GVariant *
maybe_remove_icon (GVariant *notification)
{
  GVariantBuilder n;
  int i;

  g_variant_builder_init (&n, G_VARIANT_TYPE_VARDICT);
  for (i = 0; i < g_variant_n_children (notification); i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (notification, i, "{&sv}", &key, &value);
      if (strcmp (key, "icon") != 0 || xdp_validate_serialized_icon (value, FALSE, NULL, NULL))
        g_variant_builder_add (&n, "{sv}", key, value);
    }

  return g_variant_ref_sink (g_variant_builder_end (&n));
}

static void
handle_add_in_thread_func (GTask *task,
                           gpointer source_object,
                           gpointer task_data,
                           GCancellable *cancellable)
{
  CallData *call_data = task_data;
  g_autoptr(GVariant) notification2 = NULL;

  CALL_DATA_AUTOLOCK (call_data);

  if (!xdp_app_info_is_host (call_data->app_info) &&
      !get_notification_allowed (xdp_app_info_get_id (call_data->app_info)))
    return;

  notification2 = maybe_remove_icon (call_data->notification);
  xdp_dbus_impl_notification_call_add_notification (impl,
                                                    xdp_app_info_get_id (call_data->app_info),
                                                    call_data->id,
                                                    notification2,
                                                    NULL,
                                                    add_done,
                                                    g_object_ref (call_data));
}

static gboolean
notification_handle_add_notification (XdpDbusNotification *object,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_id,
                                      GVariant *notification)
{
  Call *call = call_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  CallData *call_data;

  if (!check_notification (notification, &error))
    {
      g_prefix_error (&error, "invalid notification: ");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  call_data = call_data_new (invocation, call->app_info, call->sender, arg_id, notification);
  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, call_data, g_object_unref);
  g_task_run_in_thread (task, handle_add_in_thread_func);

  xdp_dbus_notification_complete_add_notification (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
remove_done (GObject *source,
             GAsyncResult *result,
             gpointer data)
{
  g_autoptr(CallData) call_data = data;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_notification_call_remove_notification_finish (impl, result, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }
  else
    {
      Pair p;

      p.app_id = (char *)xdp_app_info_get_id (call_data->app_info);
      p.id = call_data->id;

      G_LOCK (active);
      g_hash_table_remove (active, &p);
      G_UNLOCK (active);
    }
}

static gboolean
notification_handle_remove_notification (XdpDbusNotification *object,
                                         GDBusMethodInvocation *invocation,
                                         const char *arg_id)
{
  Call *call = call_from_invocation (invocation);
  CallData *call_data = call_data_new (invocation, call->app_info, call->sender, arg_id, NULL);

  xdp_dbus_impl_notification_call_remove_notification (impl,
                                                       xdp_app_info_get_id (call->app_info),
                                                       arg_id,
                                                       NULL,
                                                       remove_done, call_data);

  xdp_dbus_notification_complete_remove_notification (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
action_invoked (GDBusConnection *connection,
                const gchar     *sender_name,
                const gchar     *object_path,
                const gchar     *interface_name,
                const gchar     *signal_name,
                GVariant        *parameters,
                gpointer         user_data)
{
   Pair p;
   const char *action;
   GVariant *param;
   const char *sender;

   g_variant_get (parameters, "(&s&s&s@av)", &p.app_id, &p.id, &action, &param);

   sender = g_hash_table_lookup (active, &p);
   if (sender == NULL)
     return;

   g_dbus_connection_emit_signal (connection,
                                  sender,
                                  "/org/freedesktop/portal/desktop",
                                  "org.freedesktop.portal.Notification",
                                  "ActionInvoked",
                                  g_variant_new ("(ss@av)",
                                                 p.id, action,
                                                 param),
                                  NULL);

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

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      GHashTableIter iter;
      Pair *p;

      G_LOCK (active);

      g_hash_table_iter_init (&iter, active);
      while (g_hash_table_iter_next (&iter, (gpointer *)&p, NULL))
        {
          if (g_strcmp0 (p->app_id, name) == 0)
            g_hash_table_iter_remove (&iter);
        }

      G_UNLOCK (active);
    }
}

static void
notification_iface_init (XdpDbusNotificationIface *iface)
{
  iface->handle_add_notification = notification_handle_add_notification;
  iface->handle_remove_notification = notification_handle_remove_notification;
}

static void
notification_init (Notification *notification)
{
  xdp_dbus_notification_set_version (XDP_DBUS_NOTIFICATION (notification), 1);
}

static void
notification_class_init (NotificationClass *klass)
{
}

GDBusInterfaceSkeleton *
notification_create (GDBusConnection *connection,
                     const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_notification_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    dbus_name,
                                                    DESKTOP_PORTAL_OBJECT_PATH,
                                                    NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create notification proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  notification = g_object_new (notification_get_type (), NULL);
  active = g_hash_table_new_full (pair_hash, pair_equal, pair_free, g_free);

  g_dbus_connection_signal_subscribe (connection,
                                      dbus_name,
                                      "org.freedesktop.impl.portal.Notification",
                                      "ActionInvoked",
                                      DESKTOP_PORTAL_OBJECT_PATH,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      action_invoked,
                                      NULL, NULL);

  g_dbus_connection_signal_subscribe (connection,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  return G_DBUS_INTERFACE_SKELETON (notification);
}
