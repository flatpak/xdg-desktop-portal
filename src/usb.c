/*
 * Copyright © 2020 Endless OS Foundation LLC
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
 *       Ryan Gonzalez <rymg19+github@gmail.com>
 */

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib-unix.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>

#include <libudev.h>

#include "usb.h"
#include "request.h"
#include "permissions.h"
#include "session.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "usb"

typedef struct {
  gboolean   has_all_devices;
  GPtrArray *usb_rules;
} AppUsbPermissions;

static AppUsbPermissions *
app_usb_permissions_for_app_info (XdpAppInfo *app_info)
{
  AppUsbPermissions *permissions = g_new0 (AppUsbPermissions, 1);
  permissions->has_all_devices = xdp_app_info_has_all_devices (app_info);
  permissions->usb_rules = xdp_app_info_get_usb_rules (app_info);
  return permissions;
}

static void
app_usb_permissions_free (AppUsbPermissions *permissions)
{
  g_ptr_array_unref (permissions->usb_rules);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AppUsbPermissions, app_usb_permissions_free)

static GVariantIter *
get_filter_from_options (GVariantDict *options)
{
  g_autoptr(GVariantIter) filter_iter = NULL;

  if (!g_variant_dict_lookup (options, "filter", "a{sv}", &filter_iter))
    return NULL;

  return g_variant_iter_copy (g_steal_pointer (&filter_iter));
}

typedef struct _UsbSession UsbSession;
typedef struct _UsbSessionClass UsbSessionClass;

typedef struct _Usb Usb;
typedef struct _UsbClass UsbClass;

struct _UsbSession
{
  Session parent;

  AppUsbPermissions *permissions;
  GHashTable        *sessions;
  GVariantIter      *filter;
};

struct _UsbSessionClass
{
  SessionClass parent_class;
};

GType usb_session_get_type (void);

G_DEFINE_TYPE (UsbSession, usb_session, session_get_type ())

struct _Usb
{
  XdpUsbSkeleton parent_instance;

  GHashTable *ids_to_devices;
  GHashTable *syspaths_to_ids;
  GHashTable *sessions;

  struct udev         *udev;
  struct udev_monitor *monitor;
  guint                monitor_source;
};

struct _UsbClass
{
  XdpUsbSkeletonClass parent_class;
};

static XdpImplAccess *impl;
static Usb *usb;

GType usb_get_type (void) G_GNUC_CONST;
static void usb_iface_init (XdpUsbIface *iface);

G_DEFINE_TYPE_WITH_CODE (Usb, usb, XDP_TYPE_USB_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_USB, usb_iface_init));

static void
usb_session_init (UsbSession *session)
{
}

static void
usb_session_close (Session *session)
{
  g_debug ("USB session '%s' closed", session->id);
}

static void
usb_session_dispose (GObject *object)
{
  UsbSession *usb_session = (UsbSession *) object;

  if (usb_session->sessions != NULL)
    {
      g_hash_table_remove (usb_session->sessions, object);
      g_clear_pointer (&usb_session->sessions, g_hash_table_unref);
    }

  g_clear_pointer (&usb_session->permissions, app_usb_permissions_free);
  g_clear_pointer (&usb_session->filter, g_variant_iter_free);
}

static void
usb_session_class_init (UsbSessionClass *klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;
  SessionClass *session_class = (SessionClass *) klass;

  session_class->close = usb_session_close;
  object_class->dispose = usb_session_dispose;
}

static UsbSession *
usb_session_new (GVariant               *options,
                 GDBusMethodInvocation  *invocation,
                 struct udev            *udev,
                 GHashTable             *sessions,
                 GError                **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  XdpAppInfo *app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, NULL);
  Session *session = NULL;
  UsbSession *usb_session = NULL;
  g_auto(GVariantDict) options_dict;

  g_variant_dict_init (&options_dict, options);

  session = g_initable_new (usb_session_get_type (), NULL, error,
                            "sender", sender,
                            "app-id", xdp_app_info_get_id (app_info),
                            "token", lookup_session_token (options),
                            "connection", connection,
                            NULL);
  if (!session)
    return NULL;

  usb_session = (UsbSession *) session;
  usb_session->permissions = app_usb_permissions_for_app_info (app_info);
  usb_session->sessions = g_hash_table_ref (sessions);
  usb_session->filter = get_filter_from_options (&options_dict);

  g_hash_table_add (sessions, session);

  g_debug ("usb session '%s' created", session->id);

  return usb_session;
}

typedef struct udev_device UdevDevice;
typedef struct udev_enumerate UdevEnumerate;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (UdevDevice, udev_device_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (UdevEnumerate, udev_enumerate_unref)

#define UDEV_PROPERTY_BUS "ID_BUS"
#define UDEV_PROPERTY_TYPE "ID_TYPE"
#define UDEV_PROPERTY_SERIAL "ID_SERIAL"
#define UDEV_PROPERTY_SERIAL_SHORT "ID_SERIAL_SHORT"
#define UDEV_PROPERTY_VENDOR_ID "ID_VENDOR_ID"
#define UDEV_PROPERTY_VENDOR_NAME "ID_VENDOR_ENC"
#define UDEV_PROPERTY_PRODUCT_ID "ID_MODEL_ID"
#define UDEV_PROPERTY_PRODUCT_NAME "ID_MODEL_ENC"
#define UDEV_PROPERTY_INPUT_JOYSTICK "ID_INPUT_JOYSTICK"

static gboolean
is_usb_device (struct udev_device *dev)
{
  const char *bus = udev_device_get_property_value (dev, UDEV_PROPERTY_BUS);
  return g_strcmp0 (bus, "usb") == 0;
}

static gboolean
should_show_device_to_app (struct udev_device *dev,
                           AppUsbPermissions  *permissions)
{
  const char *subsystem = NULL;
  const char *product_id = NULL;
  const char *vendor_id = NULL;
  gint i;

  if (!is_usb_device (dev))
    return FALSE;

  if (permissions->has_all_devices)
    return TRUE;

  subsystem = udev_device_get_subsystem (dev);
  vendor_id = udev_device_get_property_value (dev, UDEV_PROPERTY_VENDOR_ID);
  product_id = udev_device_get_property_value (dev, UDEV_PROPERTY_PRODUCT_ID);

  for (i = 0; i < permissions->usb_rules->len; i++)
    {
      XdpUsbRule *rule = g_ptr_array_index (permissions->usb_rules, i);
      /* g_strcmp0 is used because, if for some reason we can't get the subsystem
         or vendor/product ID, it wouldn't matter anyway if the app's rules for these
         are wildcards. */
      if ((rule->subsystem == NULL || g_strcmp0 (rule->subsystem, subsystem) == 0)
          && (rule->vendor_id == NULL || g_strcmp0 (rule->vendor_id, vendor_id) == 0)
          && (rule->product_id == NULL || g_strcmp0 (rule->product_id, product_id) == 0))
        return TRUE;
    }

  return FALSE;
}

static gboolean
decode_udev_name_eval_callback (const GMatchInfo *match,
                                GString          *result,
                                gpointer          user_data)
{
  g_autofree char *digits = NULL;
  char *ep = NULL;
  gint64 value;

  digits = g_match_info_fetch (match, 1);
  g_return_val_if_fail (digits != NULL, TRUE);

  value = g_ascii_strtoll (digits, &ep, 16);
  if (*ep != '\0' || value > UCHAR_MAX || value < 0 || !isprint (value))
    {
      g_warning ("Invalid hex digits %s in %s", digits, g_match_info_get_string (match));
      value = '?';
    }

  g_string_append_c (result, value);
  return FALSE;
}

static char *
decode_udev_name (const char *name)
{
  g_autoptr(GRegex) decode_regex = NULL;
  g_autofree char *decoded = NULL;

  g_return_val_if_fail (g_utf8_validate (name, -1, NULL), NULL);

  decode_regex = g_regex_new ("\\\\x(\\d\\d)", 0, 0, NULL);
  g_return_val_if_fail (decode_regex != NULL, NULL);

  decoded = g_regex_replace_eval (decode_regex, name, -1, 0, 0,
                                  decode_udev_name_eval_callback, NULL, NULL);
  g_return_val_if_fail (decoded != NULL, NULL);

  return g_steal_pointer (&decoded);
}

static void
decode_and_insert (GVariantDict *dict,
                   const char   *key,
                   const char   *value)
{
  g_autofree char *decoded = NULL;

  decoded = decode_udev_name (value);
  if (decoded == NULL)
    {
      g_warning ("Failed to decode udev name (%s): %s", key, value);
      g_variant_dict_insert (dict, key, "s", value);
    }
  else
    g_variant_dict_insert (dict, key, "s", decoded);
}

static void
usb_fill_properties (Usb                *usb,
                     struct udev_device *dev,
                     AppUsbPermissions  *permissions,
                     GVariantDict       *out_properties)
{
  struct udev_device *parent = NULL;
  const char *devnode = NULL;
  const char *product_id = NULL;
  const char *product_name = NULL;
  const char *vendor_id = NULL;
  const char *vendor_name = NULL;
  const char *serial = NULL;
  const char *subsystem = NULL;
  const char *type = NULL;

  parent = udev_device_get_parent (dev);
  if (parent != NULL && should_show_device_to_app (parent, permissions))
    {
      const char *parent_syspath = NULL;
      const char *parent_id = NULL;

      parent_syspath = udev_device_get_syspath (parent);
      if (parent_syspath != NULL)
        {
          parent_id = g_hash_table_lookup (usb->syspaths_to_ids, parent_syspath);
          if (parent_id != NULL)
            g_variant_dict_insert (out_properties, "parent", "s", parent_id);
        }
    }

  devnode = udev_device_get_devnode (dev);
  if (devnode != NULL)
    {
      if (access (devnode, R_OK) != -1)
        g_variant_dict_insert (out_properties, "readable", "b", TRUE);
      if (access (devnode, W_OK) != -1)
        g_variant_dict_insert (out_properties, "writable", "b", TRUE);

      g_variant_dict_insert (out_properties, "devnode", "s", devnode);
    }

  product_id = udev_device_get_property_value (dev, UDEV_PROPERTY_PRODUCT_ID);
  if (product_id != NULL)
    g_variant_dict_insert (out_properties, "product_id", "s", product_id);

  vendor_id = udev_device_get_property_value (dev, UDEV_PROPERTY_VENDOR_ID);
  if (vendor_id != NULL)
    g_variant_dict_insert (out_properties, "vendor_id", "s", vendor_id);

  product_name = udev_device_get_property_value (dev, UDEV_PROPERTY_PRODUCT_NAME);
  if (product_name != NULL)
    decode_and_insert (out_properties, "product_name", product_name);

  vendor_name = udev_device_get_property_value (dev, UDEV_PROPERTY_VENDOR_NAME);
  if (vendor_name != NULL)
    decode_and_insert (out_properties, "vendor_name", vendor_name);

  // TODO: do we really want to expose this without permissions?
  serial = udev_device_get_property_value (dev, UDEV_PROPERTY_SERIAL_SHORT);
  if (serial != NULL)
    g_variant_dict_insert (out_properties, "serial", "s", serial);

  subsystem = udev_device_get_subsystem (dev);
  if (subsystem != NULL)
    g_variant_dict_insert (out_properties, "subsystem", "s", subsystem);

  type = udev_device_get_property_value (dev, UDEV_PROPERTY_TYPE);
  if (type != NULL)
    g_variant_dict_insert (out_properties, "type", "s", type);

  if (udev_device_get_property_value (dev, UDEV_PROPERTY_INPUT_JOYSTICK) != NULL)
    g_variant_dict_insert (out_properties, "has_joystick", "b", TRUE);
}

static gboolean
check_if_passes_filter (GVariantDict *properties,
                        GVariantIter *filter)
{
  g_autoptr(GVariantIter) filter_copy = NULL;

  if (filter == NULL)
    return TRUE;

  /* Copy the filter, since the loop below will modify the iterator's position. */
  filter_copy = g_variant_iter_copy (filter);

  for (;;)
    {
      const char *key = NULL;
      g_autoptr(GVariant) filter_value = NULL;
      g_autoptr(GVariant) test_value = NULL;

      if (!g_variant_iter_next (filter_copy, "{&sv}", &key, &filter_value))
        return TRUE;

      test_value = g_variant_dict_lookup_value (properties, key, NULL);
      if (test_value == NULL || !g_variant_equal (filter_value, test_value))
        return FALSE;
    }
}

static const char *
usb_create_unique_id (Usb                *usb,
                      struct udev_device *dev)
{
  g_autofree char *id = NULL;
  const char *syspath = udev_device_get_syspath (dev);

  g_return_val_if_fail (syspath != NULL, NULL);

  do
    {
      g_free (id);
      id = g_uuid_string_random ();
    }
  while (g_hash_table_contains (usb->ids_to_devices, id));

  g_debug ("Created unique ID %s -> %s", id, syspath);

  g_hash_table_insert (usb->ids_to_devices, id, udev_device_ref (dev));
  g_hash_table_insert (usb->syspaths_to_ids, g_strdup (syspath), g_strdup (id));

  return g_steal_pointer (&id);
}

static void
usb_enumerate_all_to_variant (Usb               *usb,
                              GVariantBuilder   *builder,
                              AppUsbPermissions *permissions,
                              GVariantIter      *filter)
{
  g_autoptr(UdevEnumerate) enumerator = NULL;
  struct udev_list_entry *entry = NULL;
  int r = 0;

  enumerator = udev_enumerate_new (usb->udev);
  if (enumerator == NULL)
    {
      g_warning ("Failed to create udev enumerator");
      return;
    }

  r = udev_enumerate_scan_devices (enumerator);
  if (r < 0)
    {
      g_warning ("Failed to enumerate devices: %s", strerror (-r));
      return;
    }

  for (entry = udev_enumerate_get_list_entry (enumerator); entry != NULL;
       entry = udev_list_entry_get_next (entry))
  {
    const char *syspath = NULL;
    const char *id = NULL;
    g_autoptr(UdevDevice) dev = NULL;
    g_auto(GVariantDict) properties;

    g_variant_dict_init (&properties, NULL);

    syspath = udev_list_entry_get_name (entry);
    dev = udev_device_new_from_syspath (usb->udev, syspath);
    if (dev == NULL)
      {
        g_warning ("Failed to open enumerated device %s", syspath);
        continue;
      }

    if (!should_show_device_to_app (dev, permissions))
      continue;

    id = g_hash_table_lookup (usb->syspaths_to_ids, syspath);
    if (id == NULL)
      id = usb_create_unique_id (usb, dev);

    usb_fill_properties (usb, dev, permissions, &properties);
    if (!check_if_passes_filter (&properties, filter))
      continue;

    g_variant_builder_add (builder, "{s@a{sv}}", id, g_variant_dict_end (&properties));
  }
}

static gboolean
usb_on_udev_event (int          fd,
                   GIOCondition io_condition,
                   gpointer     user_data)
{
  static const char *supported_actions[] = { "add", "change", "remove", NULL };

  Usb *usb = (Usb *) user_data;
  g_autoptr(UdevDevice) dev = NULL;
  const char *id = NULL;
  const char *action = NULL;
  const char *syspath = NULL;
  gpointer session_pointer;
  GHashTableIter iter;

  dev = udev_monitor_receive_device (usb->monitor);
  if (dev == NULL)
    {
      g_warning ("Failed to receive device from monitor");
      return G_SOURCE_CONTINUE;
    }

  if (!is_usb_device (dev))
    return G_SOURCE_CONTINUE;

  action = udev_device_get_action (dev);
  if (action == NULL)
    {
      g_warning ("Device %s had unknown action", id);
      action = "";
    }
  else if (!g_strv_contains (supported_actions, action))
    return G_SOURCE_CONTINUE;

  if (g_str_equal (action, "add"))
    id = usb_create_unique_id (usb, dev);
  else
    {
      syspath = udev_device_get_syspath (dev);
      g_return_val_if_fail (syspath != NULL, G_SOURCE_CONTINUE);

      id = g_hash_table_lookup (usb->syspaths_to_ids, syspath);
    }

  g_return_val_if_fail (id != NULL, G_SOURCE_CONTINUE);

  g_hash_table_iter_init (&iter, usb->sessions);
  while (g_hash_table_iter_next (&iter, &session_pointer, NULL))
    {
      Session *session = session_pointer;
      UsbSession *usb_session = session_pointer;
      g_auto(GVariantDict) properties;

      g_variant_dict_init (&properties, NULL);

      if (!should_show_device_to_app (dev, usb_session->permissions))
        continue;

      usb_fill_properties (usb, dev, usb_session->permissions, &properties);
      if (!check_if_passes_filter (&properties, usb_session->filter))
        continue;

      g_dbus_connection_emit_signal (session->connection,
                                     session->sender,
                                     "/org/freedesktop/portal/desktop",
                                     "org.freedesktop.portal.Usb",
                                     "DeviceEvent",
                                     g_variant_new ("(oss@a{sv})", session->id,
                                                    action, id,
                                                    g_variant_dict_end (&properties)),
                                     NULL);
    }

  if (g_str_equal (action, "remove"))
    {
      g_return_val_if_fail (syspath != NULL, G_SOURCE_CONTINUE);

      g_debug ("Removing %s -> %s", id, syspath);

      /* The value of id is owned by syspaths_to_ids, so that must be removed *after*
         the id is used for removal from ids_to_devices. */
      g_warn_if_fail (g_hash_table_remove (usb->ids_to_devices, id));
      g_warn_if_fail (g_hash_table_remove (usb->syspaths_to_ids, syspath));
    }

  return G_SOURCE_CONTINUE;
}

static const char *
get_device_permissions_key (struct udev_device *dev)
{
  const char *serial = udev_device_get_property_value (dev, UDEV_PROPERTY_SERIAL);
  g_return_val_if_fail (serial != NULL, NULL);
  return serial;
}

static char *
get_device_permissions_description (struct udev_device *dev)
{
  const char *vendor_name = udev_device_get_property_value (dev, UDEV_PROPERTY_VENDOR_NAME);
  const char *vendor_id = udev_device_get_property_value (dev, UDEV_PROPERTY_VENDOR_ID);
  const char *product_name = udev_device_get_property_value (dev, UDEV_PROPERTY_PRODUCT_NAME);
  const char *product_id = udev_device_get_property_value (dev, UDEV_PROPERTY_PRODUCT_ID);
  g_autofree char *base_description = NULL;

  g_return_val_if_fail (vendor_id != NULL && product_id != NULL, NULL);

  if (vendor_name != NULL && product_name != NULL)
    base_description = g_strdup_printf (_("%s by %s"), product_name, vendor_name);
  else if (vendor_name != NULL)
    base_description = g_strdup_printf (_("Device by %s"), vendor_name);
  else if (product_name != NULL)
    base_description = g_strdup (product_name);

  if (base_description != NULL)
    {
      const char *description = base_description;
      g_autofree char *decoded_description = decode_udev_name (description);

      if (decoded_description == NULL)
        g_warning ("Failed to decode %s", base_description);
      else
        description = decoded_description;

      return g_strdup_printf ("%s (%s:%s)", description, vendor_id, product_id);
    }
  else
    return g_strdup_printf ("%s:%s", vendor_id, product_id);
}

static void
handle_request_permission_in_thread (GTask        *task,
                                     gpointer      source_object,
                                     gpointer      task_data,
                                     GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *app_id;
  const char *parent_window;
  const char *key;
  const char *usb_description;
  Permission permission = PERMISSION_UNSET;
  gboolean allowed = FALSE;

  REQUEST_AUTOLOCK (request);

  app_id = xdp_app_info_get_id (request->app_info);

  parent_window = ((const char *)g_object_get_data (G_OBJECT (request), "parent-window"));
  key = (const char *)g_object_get_data (G_OBJECT (request), "key");
  usb_description = (const char *)g_object_get_data (G_OBJECT (request), "usb-description");

  if (xdp_app_info_has_all_devices (request->app_info))
    permission = PERMISSION_YES;
  else
    permission = get_permission_sync (app_id, PERMISSION_TABLE, key);

  if (permission == PERMISSION_YES)
    allowed = TRUE;
  else if (permission == PERMISSION_ASK || permission == PERMISSION_UNSET)
    {
      g_auto(GVariantBuilder) opt_builder;
      g_autoptr(GDesktopAppInfo) info = NULL;
      g_autofree char *subtitle = NULL;
      const char *title = NULL;
      const char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

      if (!g_str_equal (xdp_app_info_get_id (request->app_info), ""))
        {
          g_autofree char *id = NULL;

          id = g_strconcat (app_id, ".desktop", NULL);
          info = g_desktop_app_info_new (id);
        }

      title = _("Grant USB Access?");
      body = _("Access to the device can be changed at any time from the privacy settings.");

      if (info == NULL)
        subtitle = g_strdup_printf (_("An application wants to access '%s'."), usb_description);
      else
        {
          const char *name = g_app_info_get_display_name (G_APP_INFO (info));

          subtitle = g_strdup_printf (_("%s wants to access '%s'."), name, usb_description);
        }

      if (!xdp_impl_access_call_access_dialog_sync (impl,
                                                    request->id,
                                                    app_id,
                                                    "",
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&opt_builder),
                                                    &response,
                                                    &results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);

          response = 2;
          /* Make sure this result doesn't get saved later on, since it could be a fluke */
          permission = PERMISSION_ASK;
        }

      allowed = response == 0;

      if (permission == PERMISSION_UNSET)
        set_permission_sync (app_id, PERMISSION_TABLE, key, allowed ? PERMISSION_YES : PERMISSION_NO);
    }

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_request_emit_response (XDP_REQUEST (request),
                                 allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_request_permission (XdpUsb                *object,
                           GDBusMethodInvocation *invocation,
                           const char            *arg_parent_window,
                           const char            *arg_id,
                           GVariant              *arg_options)
{
  Usb *usb = (Usb *) object;
  Request *request = request_from_invocation (invocation);
  g_autoptr(AppUsbPermissions) permissions = app_usb_permissions_for_app_info (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;
  struct udev_device *dev = NULL;
  const char *key = NULL;
  g_autofree char *usb_description = NULL;

  REQUEST_AUTOLOCK (request);

  dev = g_hash_table_lookup (usb->ids_to_devices, arg_id);
  if (dev == NULL || !should_show_device_to_app (dev, permissions))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Invalid device requested");
      return TRUE;
    }

  key = get_device_permissions_key (dev);
  usb_description = get_device_permissions_description (dev);

  g_object_set_data_full (G_OBJECT (request), "key", g_strdup (key), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "usb-description", g_steal_pointer (&usb_description), g_free);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_usb_complete_request_permission (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_request_permission_in_thread);

  return TRUE;
}

static gboolean
handle_create_monitor (XdpUsb                *object,
                       GDBusMethodInvocation *invocation,
                       GVariant              *arg_options)
{
  g_autoptr(GError) error = NULL;
  UsbSession *usb_session = NULL;
  Session *session = NULL;
  Usb *usb = (Usb *) object;

  usb_session = usb_session_new (arg_options, invocation, usb->udev, usb->sessions, &error);
  if (!usb_session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  session = (Session *) usb_session;

  if (!session_export (session, &error))
    {
      g_warning ("Failed to export session: %s", error->message);
      session_close (session, FALSE);
    }
  else
    {
      g_debug ("CreateMonitor new session '%s'",  session->id);
      session_register (session);
    }

  xdp_usb_complete_create_monitor (object, invocation, session->id);
  return TRUE;
}

static gboolean
handle_enumerate_devices (XdpUsb                *object,
                          GDBusMethodInvocation *invocation,
                          GVariant              *options)
{
  Usb *usb = (Usb *) object;
  GVariantBuilder builder;
  g_autoptr(XdpAppInfo) app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, NULL);
  g_autoptr(AppUsbPermissions) permissions = NULL;
  g_autoptr(GVariantIter) filter = NULL;
  g_auto(GVariantDict) options_dict;

  g_variant_dict_init (&options_dict, options);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  filter = get_filter_from_options (&options_dict);

  permissions = app_usb_permissions_for_app_info (app_info);
  usb_enumerate_all_to_variant (usb, &builder, permissions, filter);

  xdp_usb_complete_enumerate_devices (object, invocation, g_variant_builder_end (&builder));
  return TRUE;
}

static gboolean
handle_get_device_properties (XdpUsb                *object,
                                  GDBusMethodInvocation *invocation,
                                  const char            *id)
{
  Usb *usb = (Usb *) object;
  g_autoptr(XdpAppInfo) app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, NULL);
  g_autoptr(AppUsbPermissions) permissions = app_usb_permissions_for_app_info (app_info);
  g_auto(GVariantDict) properties;
  struct udev_device *dev = NULL;

  g_variant_dict_init (&properties, NULL);

  dev = g_hash_table_lookup (usb->ids_to_devices, id);
  if (dev == NULL || !should_show_device_to_app (dev, permissions))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Invalid device requested");
      return TRUE;
    }

  usb_fill_properties (usb, dev, permissions, &properties);
  xdp_usb_complete_get_device_properties (object, invocation, g_variant_dict_end (&properties));

  return TRUE;
}

typedef struct {
  GDBusMethodInvocation *invocation;
  XdpAppInfo            *app_info;

  char     *key;
  char     *devnode;
  gboolean  writable;
} OpenDeviceTaskData;

static void
open_device_task_data_free (OpenDeviceTaskData *task_data)
{
  g_clear_object (&task_data->invocation);
  g_clear_pointer (&task_data->app_info, xdp_app_info_unref);

  g_clear_pointer (&task_data->key, g_free);
  g_clear_pointer (&task_data->devnode, g_free);
  g_free (task_data);
}

static void
open_device_in_thread_func (GTask        *task,
                            gpointer      source_object,
                            gpointer      task_data,
                            GCancellable *cancellable)
{
  OpenDeviceTaskData *open_task_data = task_data;
  GDBusMethodInvocation *invocation = g_steal_pointer (&open_task_data->invocation);
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GError) error = NULL;
  int fd;
  int index;

  if (!xdp_app_info_has_all_devices (open_task_data->app_info)
      && !get_permission_sync (xdp_app_info_get_id (open_task_data->app_info),
                               PERMISSION_TABLE,
                               open_task_data->key))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "No permission to open this device");
      return;
    }

  fd = open (open_task_data->devnode, open_task_data->writable ? O_RDWR : O_RDONLY);
  if (fd == -1)
    {
      g_warning ("Failed to open %s: %s", open_task_data->devnode, g_strerror (errno));

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to open device");
      return;
    }

  // TODO: ensure the device is still the same

  fds = g_unix_fd_list_new ();
  index = g_unix_fd_list_append (fds, fd, &error);
  close (fd);
  if (index == -1)
    {
      g_warning ("Failed to add fd for %s: %s", open_task_data->devnode, error->message);

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to open device");
      return;
    }

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           g_variant_new ("(h)", index),
                                                           g_steal_pointer (&fds));
}

static gboolean
handle_open_device (XdpUsb                *object,
                    GDBusMethodInvocation *invocation,
                    const char            *id,
                    gboolean               writable)
{
  Usb *usb = (Usb *) object;
  g_autoptr(XdpAppInfo) app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, NULL);
  g_autoptr(AppUsbPermissions) permissions = app_usb_permissions_for_app_info (app_info);
  g_autoptr(GTask) task = NULL;
  OpenDeviceTaskData *task_data = NULL;
  struct udev_device *dev = NULL;

  dev = g_hash_table_lookup (usb->ids_to_devices, id);
  if (dev == NULL || !should_show_device_to_app (dev, permissions)
      || udev_device_get_devnode (dev) == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Invalid device requested");
      return TRUE;
    }

  task_data = g_new0 (OpenDeviceTaskData, 1);
  task_data->invocation = g_object_ref (invocation);
  task_data->app_info = g_steal_pointer (&app_info);
  task_data->key = g_strdup (get_device_permissions_key (dev));
  task_data->devnode = g_strdup (udev_device_get_devnode (dev));
  task_data->writable = writable;

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, task_data, (GDestroyNotify) open_device_task_data_free);
  g_task_run_in_thread (task, open_device_in_thread_func);

  return TRUE;
}

static void
usb_iface_init (XdpUsbIface *iface)
{
  iface->handle_request_permission = handle_request_permission;
  iface->handle_create_monitor = handle_create_monitor;
  iface->handle_enumerate_devices = handle_enumerate_devices;
  iface->handle_get_device_properties = handle_get_device_properties;
  iface->handle_open_device = handle_open_device;
}

static void
usb_init_ids (Usb *device)
{
  g_autoptr(UdevEnumerate) enumerator = NULL;
  int r = 0;
  struct udev_list_entry *entry = NULL;

  enumerator = udev_enumerate_new (device->udev);
  if (enumerator == NULL)
    {
      g_warning ("Failed to create udev enumerator");
      return;
    }

  r = udev_enumerate_scan_devices (enumerator);
  if (r < 0)
    {
      g_warning ("Failed to enumerate devices: %s", strerror (-r));
      return;
    }

  for (entry = udev_enumerate_get_list_entry (enumerator); entry != NULL;
       entry = udev_list_entry_get_next (entry))
    {
      const char *syspath = NULL;
      g_autoptr(UdevDevice) dev = NULL;

      syspath = udev_list_entry_get_name (entry);
      dev = udev_device_new_from_syspath (device->udev, syspath);
      if (dev == NULL)
        {
          g_warning ("Failed to open enumerated device %s", syspath);
          continue;
        }

      if (!is_usb_device (dev))
        continue;

      usb_create_unique_id (device, dev);
    }
}

static void
usb_init (Usb *usb)
{
  xdp_usb_set_version (XDP_USB (usb), 1);

  usb->ids_to_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, (GDestroyNotify) udev_device_unref);
  usb->syspaths_to_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  usb->sessions = g_hash_table_new (g_direct_hash, g_direct_equal);

  usb->udev = udev_new ();

  // TODO: maybe set log functions?

  usb->monitor = udev_monitor_new_from_netlink (usb->udev, "udev");
  if (usb->monitor == NULL)
    g_warning ("Failed to create udev monitor");
  else
    {
      gint r = udev_monitor_enable_receiving (usb->monitor);
      if (r < 0)
        g_warning ("Failed to enable receiving udev monitor: %s", strerror (-r));
      else
        usb->monitor_source = g_unix_fd_add (udev_monitor_get_fd (usb->monitor),
                                                G_IO_IN, usb_on_udev_event, usb);
    }

  usb_init_ids (usb);
}

static void
device_dispose (GObject *object)
{
  Usb *usb = (Usb *) object;

  g_clear_pointer (&usb->ids_to_devices, g_hash_table_unref);
  g_clear_pointer (&usb->syspaths_to_ids, g_hash_table_unref);
  g_clear_pointer (&usb->sessions, g_hash_table_unref);

#ifdef HAVE_UDEV
  if (usb->monitor_source != 0)
    {
      g_source_remove (usb->monitor_source);
      usb->monitor_source = 0;
    }

  g_clear_pointer (&usb->udev, udev_unref);
#endif
}

static void
usb_class_init (UsbClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = device_dispose;
}

GDBusInterfaceSkeleton *
usb_create (GDBusConnection *connection,
            const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_access_proxy_new_sync (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         dbus_name,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         NULL,
                                         &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  usb = g_object_new (usb_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (usb);
}
