/*
 * Copyright © 2016 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
#include <gio/gunixfdlist.h>
#include <gio/gunixoutputstream.h>

#include "notification.h"
#include "call.h"
#include "permissions.h"
#include "request.h"
#include "xdp-app-info.h"
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
static guint32 impl_version;
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
  GUnixFDList *fd_list;
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
               GVariant              *notification,
               GUnixFDList           *fd_list)
{
  CallData *call_data = g_object_new (call_data_get_type(),  NULL);

  call_data->inv = g_object_ref (inv);
  call_data->app_info = g_object_ref (app_info);
  call_data->sender = g_strdup (sender);
  call_data->id = g_strdup (id);
  if (notification)
    call_data->notification = g_variant_ref (notification);
  g_set_object (&call_data->fd_list, fd_list);

  return call_data;
}

static void
call_data_finalize (GObject *object)
{
  CallData *call_data = CALL_DATA (object);

  g_clear_object (&call_data->inv);
  g_clear_object (&call_data->app_info);
  g_clear_pointer (&call_data->id, g_free);
  g_clear_pointer (&call_data->sender, g_free);
  g_clear_pointer (&call_data->notification, g_variant_unref);
  g_clear_object (&call_data->fd_list);

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

  if (!xdp_dbus_impl_notification_call_add_notification_finish (impl, NULL, result, &error))
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

static void
markup_parser_text (GMarkupParseContext  *context,
                    const gchar          *text,
                    gsize                 text_len,
                    gpointer              user_data,
                    GError              **error)
{
  GString *composed = user_data;

  g_string_append_len (composed, text, text_len);
}

static void
markup_parser_start_element (GMarkupParseContext *context,
                             const gchar         *element_name,
                             const gchar         **attribute_names,
                             const gchar         **attribute_values,
                             gpointer             user_data,
                             GError              **error)
{
  GString *composed = user_data;

  if (strcmp (element_name, "b") == 0)
    {
      g_string_append_len (composed, "<b>", -1);
    }
  else if (strcmp (element_name, "i") == 0)
    {
      g_string_append_len (composed, "<i>", -1);
    }
  else if (strcmp (element_name, "a") == 0)
    {
      int i;

      for (i = 0;  attribute_names[i]; i++)
        {
          if (strcmp (attribute_names[i], "href") == 0)
            {
              g_string_append_printf (composed, "<a href=\"%s\">", attribute_values[i]);
              break;
            }
        }
    }
}

static void
markup_parser_end_element (GMarkupParseContext *context,
                           const gchar         *element_name,
                           gpointer             user_data,
                           GError              **error)
{
  GString *composed = user_data;

  if (strcmp (element_name, "b") == 0)
    g_string_append_len (composed, "</b>", -1);
  else if (strcmp (element_name, "i") == 0)
    g_string_append_len (composed, "</i>", -1);
  else if (strcmp (element_name, "a") == 0)
    g_string_append_len (composed, "</a>", -1);
}

static const GMarkupParser markup_parser = {
  markup_parser_start_element,
  markup_parser_end_element,
  markup_parser_text,
  NULL,
  NULL,
};

static gchar *
strip_multiple_spaces (const gchar *text,
                       gsize        length)
{
  GString *composed;
  gchar *str = (gchar *) text;

  composed = g_string_sized_new (length);

  while (*str)
    {
      gunichar c = g_utf8_get_char (str);
      gboolean needs_space = FALSE;

      while (g_unichar_isspace (c))
        {
          needs_space = TRUE;

          str = g_utf8_next_char (str);

          if (!*str)
            break;

          c = g_utf8_get_char (str);
        }

      if (*str)
        {
          if (needs_space)
            g_string_append_c (composed, ' ');

          g_string_append_unichar (composed, c);
          str = g_utf8_next_char (str);
        }
    }

  return g_string_free (composed, FALSE);
}


static gboolean
parse_markup_body (GVariantBuilder  *builder,
                   GVariant         *body,
                   GError          **error)
{
  g_autoptr(GMarkupParseContext) context = NULL;
  g_autoptr(GString) composed = NULL;
  const gchar* text = NULL;
  gsize text_length = 0;

  if (!check_value_type ("markup-body", body, G_VARIANT_TYPE_STRING, error))
    return FALSE;

  text = g_variant_get_string (body, &text_length);
  composed = g_string_sized_new (text_length);
  context = g_markup_parse_context_new (&markup_parser, 0, composed, NULL);

  /* The markup parser expects the markup to start with an element, therefore add one*/
  if (g_markup_parse_context_parse (context, "<markup>", -1, error) &&
      g_markup_parse_context_parse (context, text, -1, error) &&
      g_markup_parse_context_parse (context, "</markup>", -1, error) &&
      g_markup_parse_context_end_parse (context, error))
    {
      gchar *stripped;

      stripped = strip_multiple_spaces (composed->str, composed->len);
      g_variant_builder_add (builder, "{sv}", "markup-body", g_variant_new_take_string (stripped));

      return TRUE;
    }
  else
    {
      g_prefix_error (error, "invalid markup-body: ");
      return FALSE;
    }
}

static gboolean
parse_priority (GVariantBuilder  *builder,
                GVariant         *value,
                GError          **error)
{
  const char *priorities[] = { "low", "normal", "high", "urgent", NULL };
  const char *priority;

  if (!check_value_type ("priority", value, G_VARIANT_TYPE_STRING, error))
    return FALSE;

  priority = g_variant_get_string (value, NULL);

  if (!g_strv_contains (priorities, priority))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "%s not a priority", priority);
      return FALSE;
    }

  g_variant_builder_add (builder, "{sv}", "priority", value);

  return TRUE;
}

static gboolean
check_button_purpose (GVariant  *value,
                      GError   **error)
{
  const char *purpose;
  const char *supported_purposes[] = {
    "system.custom-alert",
    "im.reply-with-text",
    "call.accept",
    "call.decline",
    "call.hang-up",
    "call.enable-speakerphone",
    "call.disable-speakerphone",
    NULL
  };

  if (!check_value_type ("purpose", value, G_VARIANT_TYPE_STRING, error))
    return FALSE;

  purpose = g_variant_get_string (value, NULL);

  if (!g_strv_contains (supported_purposes, purpose) && !g_str_has_prefix (purpose, "x-"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "%s is not a supported button purpose", purpose);
      return FALSE;
    }

  return TRUE;
}


static gboolean
parse_button (GVariantBuilder  *builder,
              GVariant         *button,
              GError          **error)
{
  int i;
  g_autoptr(GVariant) label = NULL;
  g_autoptr(GVariant) action = NULL;
  g_autoptr(GVariant) target = NULL;
  g_autoptr(GVariant) purpose = NULL;


  for (i = 0; i < g_variant_n_children (button); i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (button, i, "{&sv}", &key, &value);

      if (strcmp (key, "label") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;

          if (!label)
            label = g_steal_pointer (&value);
        }
      else if (strcmp (key, "action") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;

          if (!action)
            action = g_steal_pointer (&value);
        }
      else if (strcmp (key, "target") == 0)
        {
          if (!target)
            target = g_steal_pointer (&value);
        }
      else if (strcmp (key, "purpose") == 0 && impl_version > 1)
        {
          if (!check_button_purpose (value, error))
            return FALSE;

          if (!purpose)
            purpose = g_steal_pointer (&value);
        }
      else
        {
          g_debug ("Unsupported button property %s filtered from notification", key);
        }
    }

  if (!label && !purpose)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "label or purpose key is missing");
      return FALSE;
    }

  if (!action)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "action key is missing");
      return FALSE;
    }

  g_variant_builder_open (builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (builder, "{sv}", "label", label);
  g_variant_builder_add (builder, "{sv}", "action", action);
  if (target)
    g_variant_builder_add (builder, "{sv}", "target", target);
  if (purpose)
    g_variant_builder_add (builder, "{sv}", "purpose", purpose);

  g_variant_builder_close (builder);

  return TRUE;
}

static gboolean
parse_buttons (GVariantBuilder  *builder,
               GVariant         *value,
               GError          **error)
{
  gboolean result = TRUE;
  int i;

  if (!check_value_type ("buttons", value, G_VARIANT_TYPE ("aa{sv}"), error))
    return FALSE;

  g_variant_builder_open (builder, G_VARIANT_TYPE ("{sv}"));
  g_variant_builder_add (builder, "s", "buttons");
  g_variant_builder_open (builder, G_VARIANT_TYPE ("v"));
  g_variant_builder_open (builder, G_VARIANT_TYPE ("aa{sv}"));

  for (i = 0; i < g_variant_n_children (value); i++)
    {
      g_autoptr(GVariant) button = g_variant_get_child_value (value, i);

      if (!parse_button (builder, button, error))
        {
          g_prefix_error (error, "invalid button: ");
          result = FALSE;
          break;
        }
    }

  g_variant_builder_close (builder);
  g_variant_builder_close (builder);
  g_variant_builder_close (builder);

  return result;
}

static gboolean
parse_serialized_icon (GVariantBuilder  *builder,
                       GVariant         *icon,
                       GUnixFDList      *fd_list,
                       GError          **error)
{
  const char *key;
  g_autoptr(GVariant) value = NULL;

  /* Since the specs allow a single string as icon name we need to keep support for it */
  if (g_variant_is_of_type (icon, G_VARIANT_TYPE_STRING))
    {
      g_autoptr(GIcon) deserialized_icon = NULL;

      deserialized_icon = g_icon_deserialize (icon);

      if (G_IS_THEMED_ICON (deserialized_icon))
        {
          g_autoptr(GVariant) serialized_icon = NULL;

          serialized_icon = g_icon_serialize (deserialized_icon);

          g_variant_builder_add (builder, "{sv}", "icon", serialized_icon);
          return TRUE;
        }
      else
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                               "only themed icons can be a string");
          return FALSE;
        }
    }

  if (!check_value_type ("icon", icon, G_VARIANT_TYPE("(sv)"), error))
    return FALSE;

  g_variant_get (icon, "(&sv)", &key, &value);

  /* This are the same keys as for serialized GIcons */
  if (strcmp (key, "themed") == 0)
    {
      if (!check_value_type (key, value, G_VARIANT_TYPE_STRING_ARRAY, error))
        return FALSE;

      g_variant_builder_add (builder, "{sv}", "icon", icon);
    }
  else if (strcmp (key, "bytes") == 0)
    {
      g_autoptr(GBytes) icon_bytes = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autoptr(XdpSealedFd) sealed_icon = NULL;

      if (!check_value_type (key, value, G_VARIANT_TYPE_BYTESTRING, error))
        return FALSE;

      icon_bytes = g_variant_get_data_as_bytes (value);
      sealed_icon = xdp_sealed_fd_new_from_bytes (icon_bytes, &local_error);

      if (!sealed_icon)
        {
          g_warning ("Failed to read icon: %s", local_error->message);
        }
      else if (xdp_validate_icon (sealed_icon, XDP_ICON_TYPE_NOTIFICATION, NULL, NULL))
        {
          /* Since version 2 we only use file-descriptor icon */
          if (impl_version > 1)
            {
              g_autoptr(GVariant) fd_icon = NULL;

              fd_icon = xdp_sealed_fd_to_handle (sealed_icon, fd_list, &local_error);

              if (!fd_icon)
                g_warning ("Failed to get create file-descriptor icon from bytes icon: %s", local_error->message);

              g_variant_builder_add (builder, "{sv}", "icon", fd_icon);
            }
          else
            {
              g_variant_builder_add (builder, "{sv}", "icon", icon);
            }
        }
    }
  else if (strcmp (key, "file-descriptor") == 0)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(XdpSealedFd) sealed_icon = NULL;

      if (!check_value_type (key, value, G_VARIANT_TYPE_HANDLE, error))
        return FALSE;

      if (fd_list == NULL || g_unix_fd_list_get_length (fd_list) == 0)
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid file descriptor: No Unix FD list given or empty");
          return FALSE;
        }

      if (!(sealed_icon = xdp_sealed_fd_new_from_handle (value, fd_list, &local_error)))
        {
          g_warning ("Failed to seal fd: %s", local_error->message);
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid file descriptor: The file descriptor needs to be sealable");
          return FALSE;
        }

      if (xdp_validate_icon (sealed_icon, XDP_ICON_TYPE_NOTIFICATION, NULL, NULL))
        {
          /* Convert file descriptor icons to byte icons for backwards compatibility */
          if (impl_version < 2)
            {
                g_autoptr(GBytes) bytes = NULL;
                GVariant *bytes_icon;

                bytes = xdp_sealed_fd_get_bytes (sealed_icon, &local_error);
                if (!bytes)
                  {
                    g_warning ("Failed to get bytes from file-descriptor icon: %s", local_error->message);
                    return TRUE;
                  }

              bytes_icon = g_variant_new ("(sv)", "bytes",
                                          g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING, bytes, TRUE));

              g_variant_builder_add (builder, "{sv}", "icon", bytes_icon);
            }
          else
            {
              g_variant_builder_add (builder, "{sv}", "icon", icon);
            }
        }
    }
  else
    {
      g_debug ("Unsupported icon %s filtered from notification", key);
    }

  return TRUE;
}

static gboolean
parse_serialized_sound (GVariantBuilder  *builder,
                        GVariant         *sound,
                        GUnixFDList      *fd_list,
                        GError          **error)
{
  const char *key;
  g_autoptr(GVariant) value = NULL;

  if (g_variant_is_of_type (sound, G_VARIANT_TYPE_STRING))
    {
      key = g_variant_get_string (sound, NULL);

      if (strcmp (key, "silent") == 0 || strcmp (key, "default") == 0)
        {
          g_variant_builder_add (builder, "{sv}", "sound", sound);
          return TRUE;
        }
      else
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                               "invalid sound: invalid option");
          return FALSE;
        }
    }

  if (!check_value_type ("sound", sound, G_VARIANT_TYPE("(sv)"), error))
    return FALSE;

  g_variant_get (sound, "(&sv)", &key, &value);

  if (strcmp (key, "file-descriptor") == 0)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(XdpSealedFd) sealed_sound = NULL;

      if (!check_value_type (key, value, G_VARIANT_TYPE_HANDLE, error))
        return FALSE;

      if (fd_list == NULL || g_unix_fd_list_get_length (fd_list) == 0)
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid file descriptor: No Unix FD list given or empty");
          return FALSE;
        }

      sealed_sound = xdp_sealed_fd_new_from_handle (value, fd_list, &local_error);
      if (!sealed_sound)
        {
          g_warning ("Failed to seal fd: %s", local_error->message);
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid file descriptor: The file descriptor needs to be sealable");
          return FALSE;
        }

      if (!xdp_validate_sound (sealed_sound))
        return FALSE;

      g_variant_builder_add (builder, "{sv}", "sound", sound);
    }
  else
    {
      g_debug ("Unsupported sound %s filtered from notification", key);
    }

  return TRUE;
}

static gboolean
parse_display_hint (GVariantBuilder  *builder,
                    GVariant         *value,
                    GError          **error)
{
  int i;
  g_autofree const char **display_hints = NULL;
  gsize display_hints_length;
  const char *supported_display_hints[] = {
    "transient",
    "tray",
    "persistent",
    "hide-on-lock-screen",
    "hide-content-on-lock-screen",
    "show-as-new",
    NULL
  };

  if (!check_value_type ("display-hint", value, G_VARIANT_TYPE_STRING_ARRAY, error))
    return FALSE;

  display_hints = g_variant_get_strv (value, &display_hints_length);

  if (display_hints_length == 0)
    return TRUE;

  g_variant_builder_open (builder, G_VARIANT_TYPE ("{sv}"));
  g_variant_builder_add (builder, "s", "display-hint");
  g_variant_builder_open (builder, G_VARIANT_TYPE_VARIANT);
  g_variant_builder_open (builder, G_VARIANT_TYPE_STRING_ARRAY);

  for (i = 0; display_hints[i]; i++)
    {
      if (!g_strv_contains (supported_display_hints, display_hints[i]))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "%s not a display-hint", display_hints[i]);
          return FALSE;
        }

        g_variant_builder_add (builder, "s", display_hints[i]);
    }

  g_variant_builder_close (builder);
  g_variant_builder_close (builder);
  g_variant_builder_close (builder);

  return TRUE;
}

static gboolean
parse_category (GVariantBuilder  *builder,
                GVariant         *value,
                GError          **error)
{
  const char *category;
  const char *supported_categories[] = {
    "im.message",
    "alarm.ringing",
    "call.incoming",
    "call.ongoing",
    "call.missed",
    "weather.warning.extreme",
    "cellbroadcast.danger.extreme",
    "cellbroadcast.danger.severe",
    "cellbroadcast.amberalert",
    "cellbroadcast.test",
    "os.battery.low",
    "browser.web-notification",
    NULL
  };

  if (!check_value_type ("category", value, G_VARIANT_TYPE_STRING, error))
    return FALSE;

  category = g_variant_get_string (value, NULL);

  if (!g_strv_contains (supported_categories, category) && !g_str_has_prefix (category, "x-"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "%s is not a supported category", category);
      return FALSE;
    }

  g_variant_builder_add (builder, "{sv}", "category", value);

  return TRUE;
}

static gboolean
parse_notification (GVariantBuilder  *builder,
                    GVariant         *notification,
                    GUnixFDList      *fd_list,
                    GError          **error)
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

          g_variant_builder_add (builder, "{sv}", key, value);
        }
      else if (strcmp (key, "markup-body") == 0 && impl_version > 1)
        {
          if (!parse_markup_body (builder, value, error))
            return FALSE;
        }
      else if (strcmp (key, "icon") == 0)
        {
          if (!parse_serialized_icon (builder, value, fd_list, error))
            {
              g_prefix_error (error, "invalid icon: ");
              return FALSE;
            }
        }
      else if (strcmp (key, "sound") == 0 && impl_version > 1)
        {
          if (!parse_serialized_sound (builder, value, fd_list, error))
            {
              g_prefix_error (error, "invalid sound: ");
              return FALSE;
            }
        }
      else if (strcmp (key, "priority") == 0)
        {
          if (!parse_priority (builder, value, error))
            return FALSE;
        }
      else if (strcmp (key, "default-action") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;

          g_variant_builder_add (builder, "{sv}", key, value);
        }
      else if (strcmp (key, "default-action-target") == 0)
        {
          g_variant_builder_add (builder, "{sv}", key, value);
        }
      else if (strcmp (key, "buttons") == 0)
        {
          if (!parse_buttons (builder, value, error))
            return FALSE;
        }
      else if (strcmp (key, "display-hint") == 0 && impl_version > 1)
        {
          if (!parse_display_hint (builder, value, error))
            return FALSE;
        }
      else if (strcmp (key, "category") == 0 && impl_version > 1)
        {
          if (!parse_category (builder, value, error))
            return FALSE;
        }
      else {
        g_debug ("Unsupported property %s filtered from notification", key);
      }
    }

  return TRUE;
}

static void
add_finished_cb (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  CallData *call_data;
  g_autoptr(GError) error = NULL;

  g_assert (g_task_is_valid (result, source_object));

  call_data = g_task_get_task_data (G_TASK (result));
  g_assert (call_data != NULL);

  if (g_task_propagate_boolean (G_TASK (result), &error))
    xdp_dbus_notification_complete_add_notification (XDP_DBUS_NOTIFICATION (source_object),
                                                     call_data->inv,
                                                     NULL);
  else
    g_dbus_method_invocation_return_gerror (call_data->inv, error);
}

static void
handle_add_in_thread_func (GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data,
                           GCancellable *cancellable)
{
  CallData *call_data = task_data;
  GVariantBuilder builder;
  g_autoptr(GError) error = NULL;

  CALL_DATA_AUTOLOCK (call_data);

  if (!xdp_app_info_is_host (call_data->app_info) &&
      !get_notification_allowed (xdp_app_info_get_id (call_data->app_info)))
    {
      g_set_error_literal (&error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                           "Showing notifications is not allowed");

      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  if (!parse_notification (&builder,
                           call_data->notification,
                           call_data->fd_list,
                           &error))
    {
      g_variant_builder_clear (&builder);

      g_prefix_error (&error, "invalid notification: ");
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  xdp_dbus_impl_notification_call_add_notification (impl,
                                                    xdp_app_info_get_id (call_data->app_info),
                                                    call_data->id,
                                                    g_variant_builder_end (&builder),
                                                    call_data->fd_list,
                                                    NULL,
                                                    add_done,
                                                    g_object_ref (call_data));

  g_task_return_boolean (task, TRUE);
}

static gboolean
notification_handle_add_notification (XdpDbusNotification *object,
                                      GDBusMethodInvocation *invocation,
                                      GUnixFDList *fd_list,
                                      const char *arg_id,
                                      GVariant *notification)
{
  Call *call = call_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GUnixFDList) empty_fd_list = NULL;
  CallData *call_data;

  if (!fd_list)
    fd_list = empty_fd_list = g_unix_fd_list_new ();

  call_data = call_data_new (invocation,
                             call->app_info,
                             call->sender,
                             arg_id,
                             notification,
                             fd_list);
  task = g_task_new (object, NULL, add_finished_cb, NULL);
  g_task_set_task_data (task, call_data, g_object_unref);
  g_task_run_in_thread (task, handle_add_in_thread_func);

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
  CallData *call_data = call_data_new (invocation,
                                       call->app_info,
                                       call->sender,
                                       arg_id,
                                       NULL,
                                       NULL);

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
  xdp_dbus_notification_set_version (XDP_DBUS_NOTIFICATION (notification), 2);
  g_object_bind_property (G_OBJECT (impl), "supported-options",
                          G_OBJECT (notification), "supported-options",
                          G_BINDING_SYNC_CREATE);
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
  g_autoptr(GVariant) version = NULL;

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

  version = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (impl), "version");
  impl_version = (version != NULL) ? g_variant_get_uint32 (version) : 1;

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
