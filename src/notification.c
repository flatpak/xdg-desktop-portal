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
#include <gio/gio.h>

#include "notification.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

#define TABLE_NAME "notifications"

typedef struct _Notification Notification;
typedef struct _NotificationClass NotificationClass;

struct _Notification
{
  XdpNotificationSkeleton parent_instance;
};

struct _NotificationClass
{
  XdpNotificationSkeletonClass parent_class;
};

static XdpImplNotification *impl;
static Notification *notification;

GType notification_get_type (void) G_GNUC_CONST;
static void notification_iface_init (XdpNotificationIface *iface);

G_DEFINE_TYPE_WITH_CODE (Notification, notification, XDP_TYPE_NOTIFICATION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_NOTIFICATION, notification_iface_init));

static void
add_done (GObject *source,
          GAsyncResult *result,
          gpointer data)
{
  g_autoptr(Request) request = data;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_notification_call_add_notification_finish (impl, result, &error))
    g_warning ("Backend call failed: %s", error->message);
}

static gboolean
get_notification_allowed (const char *app_id)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   "notification",
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error getting permissions: %s", error->message);
      return TRUE;
    }

  if (out_perms != NULL)
    {
      const char **perms;
      if (g_variant_lookup (out_perms, app_id, "^a&s", &perms))
        return !g_strv_contains (perms, "no");
    }

  return TRUE;
}


static void
handle_add_in_thread_func (GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data,
                           GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *id;
  GVariant *notification;

  REQUEST_AUTOLOCK (request);

  if (!get_notification_allowed (request->app_id))
    return;

  id = (const char *)g_object_get_data (G_OBJECT (request), "id");
  notification = (GVariant *)g_object_get_data (G_OBJECT (request), "notification");

  xdp_impl_notification_call_add_notification (impl,
                                               request->app_id,
                                               id,
                                               notification,
                                               NULL,
                                               add_done,
                                               g_object_ref (request));
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
               G_IO_ERROR, G_IO_ERROR_FAILED,
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
                   G_IO_ERROR, G_IO_ERROR_FAILED,
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
                       G_IO_ERROR, G_IO_ERROR_FAILED,
                       "%s not valid key", key);
          return FALSE;
        }
    }

  if (!has_label)
    {
      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_FAILED,
                   "label key is missing");
      return FALSE;
    }

  if (!has_action)
    {
      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_FAILED,
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
  g_autoptr(GIcon) icon = NULL;

  if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING) ||
      g_variant_is_of_type (value, G_VARIANT_TYPE("(sv)")))
    icon = g_icon_deserialize (value);

  if (!icon)
    {
      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_FAILED,
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
                       G_IO_ERROR, G_IO_ERROR_FAILED,
                       "%s not valid key", key);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
notification_handle_add_notification (XdpNotification *object,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_id,
                                      GVariant *notification)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;

  g_object_set_data_full (G_OBJECT (request), "id", g_strdup (arg_id), g_free);
  g_object_set_data_full (G_OBJECT (request), "notification", g_variant_ref (notification), (GDestroyNotify)g_variant_unref);

  if (!check_notification (notification, &error))
    {
      g_prefix_error (&error, "invalid notification: ");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_add_in_thread_func);

  xdp_notification_complete_add_notification (object, invocation);

  return TRUE;
}

static gboolean
notification_handle_remove_notification (XdpNotification *object,
                                         GDBusMethodInvocation *invocation,
                                         const char *arg_id)
{
  g_autoptr(Request) request = request_from_invocation (invocation);

  xdp_impl_notification_call_remove_notification (impl,
                                                  request->app_id,
                                                  arg_id,
                                                  NULL,
                                                  NULL, NULL);

  xdp_notification_complete_remove_notification (object, invocation);

  return TRUE;
}

static void
notification_class_init (NotificationClass *klass)
{
}

static void
notification_iface_init (XdpNotificationIface *iface)
{
  iface->handle_add_notification = notification_handle_add_notification;
  iface->handle_remove_notification = notification_handle_remove_notification;
}

static void
notification_init (Notification *resolver)
{
}

GDBusInterfaceSkeleton *
notification_create (GDBusConnection *connection,
                     const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_notification_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               "/org/freedesktop/portal/desktop",
                                               NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create notification proxy: %s", error->message);
      return NULL;
    }

  notification = g_object_new (notification_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (notification);
}
