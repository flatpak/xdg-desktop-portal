/*
 * Copyright © 2023-2024 GNOME Foundation Inc.
 *             2020 Endless OS Foundation LLC
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
 *       Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *       Hubert Figuière <hub@figuiere.net>
 *       Ryan Gonzalez <rymg19+github@gmail.com>
 */

#include "config.h"

#include <ctype.h>
#include <stdint.h>
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
#include <gudev/gudev.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-session.h"
#include "xdp-utils.h"
#include "xdp-usb-query.h"

#include "usb.h"

#define PERMISSION_TABLE "usb"
#define PERMISSION_ID "usb"
#define MAX_DEVICES 8

/* TODO:
 *
 * AccessDevices()
 *  - Check if backend is returning appropriate device ids
 *  - Check if backend is not increasing permissions
 *  - Save allowed devices in the permission store
 */

struct _XdpUsb
{
  XdpDbusUsbSkeleton parent_instance;

  GHashTable *ids_to_devices;
  GHashTable *syspaths_to_ids;

  GHashTable *sessions;
  GHashTable *sender_infos;

  GUdevClient *gudev_client;
};

#define XDP_TYPE_USB (xdp_usb_get_type ())
G_DECLARE_FINAL_TYPE (XdpUsb, xdp_usb, XDP, USB, XdpDbusUsbSkeleton)

static void xdp_usb_iface_init (XdpDbusUsbIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpUsb, xdp_usb, XDP_DBUS_TYPE_USB_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_USB, xdp_usb_iface_init));

struct _XdpUsbSession
{
  XdpSession parent;

  GHashTable *available_devices;
};

#define XDP_TYPE_USB_SESSION (xdp_usb_session_get_type ())
G_DECLARE_FINAL_TYPE (XdpUsbSession,
                      xdp_usb_session,
                      XDP, USB_SESSION,
                      XdpSession)

G_DEFINE_TYPE (XdpUsbSession, xdp_usb_session, xdp_session_get_type ())

typedef struct
{
  char *device_id;
  gboolean writable;
} UsbDeviceAcquireData;

typedef struct _UsbOwnedDevice
{
  gatomicrefcount ref_count;

  char *sender_name;
  char *device_id;
  int fd;
} UsbOwnedDevice;

typedef struct _UsbSenderInfo
{
  gatomicrefcount ref_count;

  char *sender_name;
  XdpAppInfo *app_info;

  GHashTable *pending_devices; /* object_path → GPtrArray */

  GHashTable *owned_devices; /* device id → UsbOwnedDevices */
} UsbSenderInfo;

static XdpDbusImplUsb *usb_impl;
static XdpUsb *usb;

static void usb_device_acquire_data_free (UsbDeviceAcquireData *acquire_data);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (UsbDeviceAcquireData, usb_device_acquire_data_free)

static void usb_owned_device_unref (UsbOwnedDevice *owned_device);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (UsbOwnedDevice, usb_owned_device_unref)

static void usb_sender_info_unref (UsbSenderInfo *sender_info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (UsbSenderInfo, usb_sender_info_unref)

static gboolean
is_gudev_device_suitable (GUdevDevice *device)
{
  const char *device_file = NULL;
  const char *devtype = NULL;

  g_assert (G_UDEV_IS_DEVICE (device));
  g_return_val_if_fail (g_strcmp0 (g_udev_device_get_subsystem (device), "usb") == 0,
                        FALSE);

  device_file = g_udev_device_get_device_file (device);
  if (!device_file)
    return FALSE;

  devtype = g_udev_device_get_property (device, "DEVTYPE");
  if (!devtype || g_strcmp0 (devtype, "usb_device") != 0)
    return FALSE;

  return TRUE;
}

static char *
unique_permission_id_for_device (GUdevDevice *device)
{
  g_autoptr(GString) permission_id = NULL;
  const char *property;

  permission_id = g_string_new ("usb:");

  property = g_udev_device_get_property (device, "ID_VENDOR_ID");
  if (property)
    g_string_append (permission_id, property);

  property = g_udev_device_get_property (device, "ID_MODEL_ID");
  g_string_append_printf (permission_id, "%s%s", "/", property ? property : "");

  property = g_udev_device_get_property (device, "ID_SERIAL");
  g_string_append_printf (permission_id, "%s%s", "/", property ? property : "");

  return g_string_free (g_steal_pointer (&permission_id), FALSE);
}

static void
usb_device_acquire_data_free (UsbDeviceAcquireData *acquire_data)
{
  g_return_if_fail (acquire_data != NULL);

  g_clear_pointer (&acquire_data->device_id, g_free);
  g_clear_pointer (&acquire_data, g_free);
}

static UsbOwnedDevice *
usb_owned_device_ref (UsbOwnedDevice *owned_device)
{
  g_return_val_if_fail (owned_device != NULL, NULL);

  g_atomic_ref_count_inc (&owned_device->ref_count);

  return owned_device;
}

static void
usb_owned_device_unref (UsbOwnedDevice *owned_device)
{
  g_return_if_fail (owned_device != NULL);

  if (g_atomic_ref_count_dec (&owned_device->ref_count))
    {
      g_clear_fd (&owned_device->fd, NULL);
      g_clear_pointer (&owned_device->device_id, g_free);
      g_clear_pointer (&owned_device, g_free);
    }
}

static void
usb_sender_info_unref (UsbSenderInfo *sender_info)
{
  g_return_if_fail (sender_info != NULL);

  if (g_atomic_ref_count_dec (&sender_info->ref_count))
    {
      g_clear_object (&sender_info->app_info);
      g_clear_pointer (&sender_info->sender_name, g_free);
      g_clear_pointer (&sender_info->owned_devices, g_hash_table_destroy);
      g_clear_pointer (&sender_info->pending_devices, g_hash_table_destroy);
      g_clear_pointer (&sender_info, g_free);
    }
}

static UsbSenderInfo *
usb_sender_info_new (const char *sender_name,
                     XdpAppInfo *app_info)
{
  g_autoptr(UsbSenderInfo) sender_info = NULL;

  sender_info = g_new0 (UsbSenderInfo, 1);
  g_atomic_ref_count_init (&sender_info->ref_count);
  sender_info->sender_name = g_strdup (sender_name);
  sender_info->app_info = g_object_ref (app_info);
  sender_info->owned_devices =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) usb_owned_device_unref);
  sender_info->pending_devices =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) g_ptr_array_unref);

  return g_steal_pointer (&sender_info);
}

static UsbSenderInfo *
usb_sender_info_from_app_info (XdpUsb     *self,
                               XdpAppInfo *app_info)
{
  const char *sender = xdp_app_info_get_sender (app_info);
  g_autoptr(UsbSenderInfo) sender_info = NULL;

  sender_info = g_hash_table_lookup (self->sender_infos, sender);

  if (!sender_info)
    {
      sender_info = usb_sender_info_new (sender, app_info);
      g_hash_table_insert (self->sender_infos, g_strdup (sender), sender_info);
    }

  g_assert (sender_info != NULL);
  g_atomic_ref_count_inc (&sender_info->ref_count);

  return g_steal_pointer (&sender_info);
}

static UsbSenderInfo *
usb_sender_info_from_request (XdpUsb     *self,
                              XdpRequest *request)
{
  g_return_val_if_fail (request != NULL, NULL);

  return usb_sender_info_from_app_info (self, request->app_info);
}

static void
usb_sender_info_acquire_device (UsbSenderInfo *sender_info,
                                const char    *device_id,
                                int            fd)
{
  g_autoptr(UsbOwnedDevice) owned_device = NULL;

  g_assert (sender_info != NULL);
  g_assert (!g_hash_table_contains (sender_info->owned_devices, device_id));

  owned_device = g_new0 (UsbOwnedDevice, 1);
  g_atomic_ref_count_init (&owned_device->ref_count);
  owned_device->device_id = g_strdup (device_id);
  owned_device->fd = g_steal_fd (&fd);

  g_hash_table_insert (sender_info->owned_devices,
                       g_strdup (device_id),
                       g_steal_pointer (&owned_device));
}

static void
usb_sender_info_release_device (UsbSenderInfo *sender_info,
                                const char    *device_id)
{
  g_assert (sender_info != NULL);

  if (!g_hash_table_remove (sender_info->owned_devices, device_id))
    g_warning ("Device %s not owned by %s", device_id, sender_info->sender_name);

}

static XdpPermission
usb_sender_info_get_device_permission (UsbSenderInfo *sender_info,
                                       GUdevDevice   *device)
{
  g_autofree char *permission_id = NULL;

  g_assert (G_UDEV_IS_DEVICE (device));

  permission_id = unique_permission_id_for_device (device);
  return xdp_get_permission_sync (sender_info->app_info,
                                  PERMISSION_TABLE, permission_id);
}

static void
usb_sender_info_set_device_permission (UsbSenderInfo *sender_info,
                                       GUdevDevice   *device,
                                       XdpPermission     permission)
{
  g_autofree char *permission_id = NULL;

  g_assert (G_UDEV_IS_DEVICE (device));

  permission_id = unique_permission_id_for_device (device);
  xdp_set_permission_sync (sender_info->app_info,
                           PERMISSION_TABLE, permission_id, permission);
}

static gboolean
usb_sender_info_match_device (UsbSenderInfo *sender_info,
                              GUdevDevice   *device)
{
  const char *device_subclass_str = NULL;
  const char *device_class_str = NULL;
  const char *product_id_str = NULL;
  const char *vendor_id_str = NULL;
  gboolean device_has_product_id = FALSE;
  gboolean device_has_vendor_id = FALSE;
  gboolean device_has_subclass = FALSE;
  gboolean device_has_class = FALSE;
  XdpPermission permission;
  uint16_t device_product_id;
  uint16_t device_vendor_id;
  uint16_t device_subclass;
  uint16_t device_class;
  gboolean match = FALSE;
  const GPtrArray *queries = NULL;

  permission = usb_sender_info_get_device_permission (sender_info, device);
  if (permission == XDP_PERMISSION_NO)
    return FALSE;

  vendor_id_str = g_udev_device_get_property (device, "ID_VENDOR_ID");
  if (vendor_id_str != NULL && xdp_validate_hex_uint16 (vendor_id_str, 4, &device_vendor_id))
    device_has_vendor_id = TRUE;

  product_id_str = g_udev_device_get_property (device, "ID_MODEL_ID");
  if (product_id_str != NULL && xdp_validate_hex_uint16 (product_id_str, 4, &device_product_id))
    device_has_product_id = TRUE;

  device_class_str = g_udev_device_get_sysfs_attr (device, "bDeviceClass");
  if (device_class_str != NULL && xdp_validate_hex_uint16 (device_class_str, 2, &device_class))
    device_has_class = TRUE;

  device_subclass_str = g_udev_device_get_sysfs_attr (device, "bDeviceSubclass");
  if (device_subclass_str != NULL && xdp_validate_hex_uint16 (device_subclass_str, 2, &device_subclass))
    device_has_subclass = TRUE;

  queries = xdp_app_info_get_usb_queries (sender_info->app_info);
  for (size_t i = 0; queries && i < queries->len; i++)
    {
      XdpUsbQuery *query = g_ptr_array_index (queries, i);
      gboolean query_matches = TRUE;

      if (!query)
        {
          g_debug ("query %ld is null", i);
          continue;
        }

      for (size_t j = 0; j < query->rules->len; j++)
        {
          XdpUsbRule *rule = g_ptr_array_index (query->rules, j);

          switch (rule->rule_type)
            {
            case XDP_USB_RULE_TYPE_ALL:
              query_matches = TRUE;
              break;

            case XDP_USB_RULE_TYPE_CLASS:
              query_matches &= device_has_class &&
                               device_class == rule->d.device_class.class;

              if (rule->d.device_class.type == XDP_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS)
                query_matches &= device_has_subclass &&
                                 device_subclass == rule->d.device_class.subclass;

              break;

            case XDP_USB_RULE_TYPE_DEVICE:
              query_matches &= device_has_product_id &&
                               device_product_id == rule->d.product.id;
              break;

            case XDP_USB_RULE_TYPE_VENDOR:
              query_matches &= device_has_vendor_id &&
                               device_vendor_id == rule->d.vendor.id;
              break;

            default:
              g_assert_not_reached ();
            }
        }

      switch (query->query_type)
        {
        case XDP_USB_QUERY_TYPE_ENUMERABLE:
          if (query_matches)
            match = TRUE;
          break;

        case XDP_USB_QUERY_TYPE_HIDDEN:
          if (query_matches)
            return FALSE;
          break;
        }
    }

  return match;
}

static void
xdp_usb_session_close (XdpSession *session)
{
  g_debug ("USB session '%s' closed", session->id);

  g_assert (g_hash_table_contains (usb->sessions, session));
  g_hash_table_remove (usb->sessions, session);
}

static void
xdp_usb_session_dispose (GObject *object)
{
  XdpUsbSession *usb_session = XDP_USB_SESSION (object);

  g_clear_pointer (&usb_session->available_devices, g_hash_table_destroy);
}

static void
xdp_usb_session_class_init (XdpUsbSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpSessionClass *session_class = (XdpSessionClass*) klass;

  object_class->dispose = xdp_usb_session_dispose;

  session_class->close = xdp_usb_session_close;
}

static void
xdp_usb_session_init (XdpUsbSession *session)
{
  session->available_devices = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static XdpUsbSession *
xdp_usb_session_new (GDBusConnection  *connection,
                     XdpAppInfo       *app_info,
                     GVariant         *options,
                     GError          **error)
{
  XdpSession *session = NULL;

  session = g_initable_new (XDP_TYPE_USB_SESSION,
                            NULL, error,
                            "connection", connection,
                            "sender", xdp_app_info_get_sender (app_info),
                            "app-id", xdp_app_info_get_id (app_info),
                            "token", lookup_session_token (options),
                            NULL);
  if (!session)
    return NULL;

  g_debug ("[usb] USB session '%s' created", session->id);

  return XDP_USB_SESSION (session);
}

static GVariant *
gudev_device_to_variant (XdpUsb        *self,
                         UsbSenderInfo *sender_info,
                         GUdevDevice   *device)
{
  g_auto(GVariantDict) udev_properties_dict = G_VARIANT_DICT_INIT (NULL);
  g_auto(GVariantDict) device_variant_dict = G_VARIANT_DICT_INIT (NULL);
  g_autoptr(GUdevDevice) parent = NULL;
  const char *device_file = NULL;
  size_t n_added_properties = 0;

  static const char * const allowed_udev_properties[] = {
    "ID_INPUT_JOYSTICK",
    "ID_MODEL_ID",
    "ID_MODEL_ENC",
    "ID_MODEL_FROM_DATABASE",
    "ID_REVISION",
    "ID_SERIAL",
    "ID_SERIAL_SHORT",
    "ID_TYPE",
    "ID_VENDOR_ENC",
    "ID_VENDOR_ID",
    "ID_VENDOR_FROM_DATABASE",
    NULL,
  };

  if (!is_gudev_device_suitable (device))
    return NULL;

  parent = g_udev_device_get_parent (device);
  if (parent != NULL && usb_sender_info_match_device (sender_info, parent))
    {
      const char *parent_syspath = NULL;
      const char *parent_id = NULL;

      parent_syspath = g_udev_device_get_sysfs_path (parent);
      if (parent_syspath != NULL)
        {
          parent_id = g_hash_table_lookup (self->syspaths_to_ids, parent_syspath);
          if (parent_id != NULL)
            g_variant_dict_insert (&device_variant_dict, "parent", "s", parent_id);
        }
    }

  device_file = g_udev_device_get_device_file (device);
  g_variant_dict_insert (&device_variant_dict, "device-file", "s", device_file);

  if (access (device_file, R_OK) != -1)
    g_variant_dict_insert (&device_variant_dict, "readable", "b", TRUE);
  if (access (device_file, W_OK) != -1)
    g_variant_dict_insert (&device_variant_dict, "writable", "b", TRUE);

  for (size_t i = 0; allowed_udev_properties[i] != NULL; i++)
    {
      const char *property = g_udev_device_get_property (device, allowed_udev_properties[i]);

      if (!property)
        continue;

      g_variant_dict_insert (&udev_properties_dict, allowed_udev_properties[i], "s", property);
      n_added_properties++;
    }

  if (n_added_properties > 0)
    {
      g_variant_dict_insert (&device_variant_dict,
                             "properties",
                             "@a{sv}",
                             g_variant_dict_end (&udev_properties_dict));
    }

  return g_variant_ref_sink (g_variant_dict_end (&device_variant_dict));
}

/* Register the device and create a unique ID for it */
static char *
register_with_unique_usb_id (XdpUsb       *self,
                             GUdevDevice  *device)
{
  g_autofree char *id = NULL;
  const char *syspath;

  g_assert (is_gudev_device_suitable (device));

  syspath = g_udev_device_get_sysfs_path (device);
  g_assert (syspath != NULL);

  do
    {
      g_clear_pointer (&id, g_free);
      id = g_uuid_string_random ();
    }
  while (g_hash_table_contains (self->ids_to_devices, id));

  g_debug ("Assigned unique ID %s to USB device %s", id, syspath);

  g_hash_table_insert (self->ids_to_devices, g_strdup (id), g_object_ref (device));
  g_hash_table_insert (self->syspaths_to_ids, g_strdup (syspath), g_strdup (id));

  return g_steal_pointer (&id);
}

static void
handle_session_event (XdpUsb        *self,
                      XdpUsbSession *usb_session,
                      GUdevDevice   *device,
                      const char    *id,
                      const char    *action,
                      gboolean       removing)
{
  g_autoptr(GVariant) device_variant = NULL;
  GVariantBuilder devices_builder;
  UsbSenderInfo *sender_info;
  XdpSession *session;

  g_assert (G_UDEV_IS_DEVICE (device));
  g_assert (g_strcmp0 (g_udev_device_get_subsystem (device), "usb") == 0);

  session = XDP_SESSION (usb_session);
  sender_info = g_hash_table_lookup (self->sender_infos, session->sender);
  g_assert (sender_info != NULL);

  /* We can't use usb_sender_info_match_device() when a device is being removed because,
   * on removal, the only property the GUdevDevice has is its sysfs path.
   * Check if this device was previously available to the USB session
   * instead. */
  if ((removing && !g_hash_table_contains (usb_session->available_devices, id)) ||
      (!removing && !usb_sender_info_match_device (sender_info, device)))
    return;

  g_variant_builder_init (&devices_builder, G_VARIANT_TYPE ("a(ssa{sv})"));

  device_variant = gudev_device_to_variant (self, sender_info, device);
  g_variant_builder_add (&devices_builder, "(ss@a{sv})", action, id, device_variant);

  g_dbus_connection_emit_signal (session->connection,
                                 session->sender,
                                 "/org/freedesktop/portal/desktop",
                                 "org.freedesktop.portal.Usb",
                                 "DeviceEvents",
                                 g_variant_new ("(o@a(ssa{sv}))",
                                                session->id,
                                                g_variant_builder_end (&devices_builder)),
                                 NULL);

  if (removing)
    g_hash_table_remove (usb_session->available_devices, id);
  else
    g_hash_table_add (usb_session->available_devices, g_strdup (id));
}

static void
gudev_client_uevent_cb (GUdevClient *client,
                        const char  *action,
                        GUdevDevice *device,
                        XdpUsb      *self)
{
  static const char *supported_actions[] = {
    "add",
    "change",
    "remove",
    NULL,
  };

  g_autofree char *id = NULL;
  GHashTableIter iter;
  XdpUsbSession *usb_session;
  const char *syspath = NULL;
  gboolean removing;

  if (!g_strv_contains (supported_actions, action))
    return;

  if (!is_gudev_device_suitable (device))
    return;

  removing = g_str_equal (action, "remove");

  if (g_str_equal (action, "add"))
    {
      id = register_with_unique_usb_id (self, device);
    }
  else
    {
      syspath = g_udev_device_get_sysfs_path (device);

      g_assert (syspath != NULL);
      id = g_strdup (g_hash_table_lookup (self->syspaths_to_ids, syspath));
    }

  g_assert (id != NULL);

  /* Send event to all sessions that are allowed to handle it */
  g_hash_table_iter_init (&iter, self->sessions);
  while (g_hash_table_iter_next (&iter, (gpointer *) &usb_session, NULL))
    handle_session_event (self, usb_session, device, id, action, removing);

  if (removing)
    {
      g_assert (syspath != NULL);

      g_debug ("Removing %s -> %s", id, syspath);

      /* The value of id is owned by syspaths_to_ids, so that must be removed *after*
         the id is used for removal from ids_to_devices. */
      if (!g_hash_table_remove (self->ids_to_devices, id))
        g_critical ("Error removing USB device from ids_to_devices table");

      if (!g_hash_table_remove (self->syspaths_to_ids, syspath))
        g_critical ("Error removing USB device from syspaths_to_ids table");
    }
}

static void
send_initial_device_list (XdpUsb        *self,
                          XdpUsbSession *usb_session,
                          XdpAppInfo    *app_info)
{
  /* Send initial list of devices the app has permission to see */
  g_autoptr(UsbSenderInfo) sender_info = NULL;
  XdpSession *session = XDP_SESSION (usb_session);
  GVariantBuilder devices_builder;
  g_autoptr(GVariant) events = NULL;
  GHashTableIter iter;
  GUdevDevice *device;
  const char *id;
  gboolean has_devices = FALSE;

  g_debug ("[usb] Appending devices to CreateSession response");

  g_variant_builder_init (&devices_builder, G_VARIANT_TYPE ("(oa(ssa{sv}))"));
  g_variant_builder_add (&devices_builder, "o", session->id);
  g_variant_builder_open (&devices_builder, G_VARIANT_TYPE ("a(ssa{sv})"));

  g_assert (self != NULL);

  sender_info = usb_sender_info_from_app_info (self, app_info);

  g_hash_table_iter_init (&iter, self->ids_to_devices);
  while (g_hash_table_iter_next (&iter, (gpointer *) &id, (gpointer *) &device))
    {
      g_autoptr(GVariant) device_variant = NULL;

      g_assert (G_UDEV_IS_DEVICE (device));
      g_assert (g_strcmp0 (g_udev_device_get_subsystem (device), "usb") == 0);

      if (!usb_sender_info_match_device (sender_info, device))
        continue;

      device_variant = gudev_device_to_variant (self, sender_info, device);
      /* NULL mean the device isn't suitable */
      if (device_variant == NULL)
        continue;

      g_variant_builder_add (&devices_builder, "(ss@a{sv})", "add", id, device_variant);

      g_hash_table_add (usb_session->available_devices, g_strdup (id));

      has_devices = TRUE;
    }

  g_variant_builder_close (&devices_builder);
  events = g_variant_ref_sink (g_variant_builder_end (&devices_builder));

  if (!has_devices)
    return;

  g_dbus_connection_emit_signal (session->connection,
                                 session->sender,
                                 "/org/freedesktop/portal/desktop",
                                 "org.freedesktop.portal.Usb",
                                 "DeviceEvents",
                                 events,
                                 NULL);
}

static gboolean
handle_create_session (XdpDbusUsb            *object,
                       GDBusMethodInvocation *invocation,
                       GVariant              *arg_options)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  GDBusConnection *connection;
  GVariantBuilder options_builder;
  XdpUsbSession *usb_session;
  XdpPermission permission;
  XdpSession *session;
  XdpUsb *self;

  static const XdpOptionKey usb_create_session_options[] = {
    { "session_handle_token", G_VARIANT_TYPE_STRING, NULL },
  };

  self = XDP_USB (object);

  g_debug ("[usb] Handling CreateSession");

  permission = xdp_get_permission_sync (app_info,
                                        PERMISSION_TABLE,
                                        PERMISSION_ID);
  if (permission == XDP_PERMISSION_NO)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed to create USB sessions");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options,
                           &options_builder,
                           usb_create_session_options,
                           G_N_ELEMENTS (usb_create_session_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  connection = g_dbus_method_invocation_get_connection (invocation);
  usb_session = xdp_usb_session_new (connection, app_info, options, &error);
  if (!usb_session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = XDP_SESSION (usb_session);
  if (!xdp_session_export (session, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      xdp_session_close (session, FALSE);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_session_register (session);

  g_debug ("New USB session registered: %s",  session->id);
  g_hash_table_add (self->sessions, usb_session);

  xdp_dbus_usb_complete_create_session (object, invocation, session->id);

  send_initial_device_list (self, usb_session, app_info);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static GVariant *
list_permitted_devices (XdpUsb     *self,
                        XdpAppInfo *app_info)
{
  g_autoptr(UsbSenderInfo) sender_info = NULL;
  GVariantBuilder builder;
  GHashTableIter iter;
  GUdevDevice *device;
  const char *id;

  sender_info = usb_sender_info_from_app_info (self, app_info);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));

  g_hash_table_iter_init (&iter, self->ids_to_devices);
  while (g_hash_table_iter_next (&iter, (gpointer *) &id, (gpointer *) &device))
    {
      g_assert (G_UDEV_IS_DEVICE (device));
      g_assert (g_strcmp0 (g_udev_device_get_subsystem (device), "usb") == 0);

      if (usb_sender_info_match_device (sender_info, device))
        {
          g_autoptr(GVariant) device_variant = gudev_device_to_variant (self, sender_info, device);
          if (device_variant == NULL)
            continue;

          g_variant_builder_add (&builder, "(s@a{sv})", id, device_variant);
        }
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* List devices the app has permission */
static gboolean
handle_enumerate_devices (XdpDbusUsb            *object,
                          GDBusMethodInvocation *invocation,
                          GVariant              *arg_options)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) devices = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  XdpPermission permission;
  XdpUsb *self;

  static const XdpOptionKey usb_enumerate_devices_options[] = {
  };

  self = XDP_USB (object);

  permission = xdp_get_permission_sync (app_info,
                                        PERMISSION_TABLE,
                                        PERMISSION_ID);

  if (permission == XDP_PERMISSION_NO)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed to enumerate devices");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           usb_enumerate_devices_options,
                           G_N_ELEMENTS (usb_enumerate_devices_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  devices = list_permitted_devices (self, app_info);

  xdp_dbus_usb_complete_enumerate_devices (object, invocation, devices);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
usb_acquire_devices_cb (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      data)
{
  XdgDesktopPortalResponseEnum response;
  g_autoptr(UsbSenderInfo) sender_info = NULL;
  g_autoptr(GVariantIter) devices_iter = NULL;
  g_auto(GVariantBuilder) results_builder;
  g_autoptr (GVariant) results = NULL;
  g_autoptr(XdpRequest) request = data;
  g_autoptr(GError) error = NULL;
  GVariant *options;
  const char *device_id;

  REQUEST_AUTOLOCK (request);

  response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  sender_info = usb_sender_info_from_request (usb, request);

  g_assert (sender_info != NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_dbus_impl_usb_call_acquire_devices_finish (usb_impl, &response, &results, result, &error))
    {
      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      g_dbus_error_strip_remote_error (error);
      goto out;
    }

  /* TODO: check if the list of devices that the backend reported is strictly
   * equal or a subset of the devices the app requested. */

  /* TODO: check if we're strictly equal or downgrading the "writable" option */

  if (!g_variant_lookup (results, "devices", "a(sa{sv})", &devices_iter))
    goto out;

  if (response == XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    {
      g_autoptr(GPtrArray) pending_devices =
        g_ptr_array_new_full (g_variant_iter_n_children (devices_iter),
                              (GDestroyNotify) usb_device_acquire_data_free);
      while (g_variant_iter_next (devices_iter, "(&s@a{sv})", &device_id, &options))
        {
          g_autoptr(UsbDeviceAcquireData) access_data = NULL;
          GUdevDevice *device;
          gboolean writable;

          device = g_hash_table_lookup (usb->ids_to_devices, device_id);
          if (!device)
            continue;

          if (!g_variant_lookup (options, "writable", "b", &writable))
            writable = FALSE;

          access_data = g_new0 (UsbDeviceAcquireData, 1);
          access_data->device_id = g_strdup (device_id);
          access_data->writable = writable;

          g_ptr_array_add (pending_devices, g_steal_pointer (&access_data));

          usb_sender_info_set_device_permission (sender_info, device, XDP_PERMISSION_YES);

          g_clear_pointer (&options, g_variant_unref);
        }
      g_hash_table_insert (sender_info->pending_devices,
                           g_strdup (xdp_request_get_object_path (request)),
                           g_steal_pointer (&pending_devices));
    }
  else if (response == XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED)
    {
      g_hash_table_remove (sender_info->pending_devices,
                           xdp_request_get_object_path (request));
    }

out:
  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&results_builder));
      xdp_request_unexport (request);
    }
}

static gboolean
filter_access_devices_writable (const char   *key,
                                GVariant     *value,
                                GUdevDevice  *device,
                                GError      **error)
{
  const char *device_file;
  gboolean writable = g_variant_get_boolean (value);

  if (!writable)
    return TRUE;

  device_file = g_udev_device_get_device_file (device);
  if (access (device_file, W_OK) != -1)
    return TRUE;

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
               "Requested writable access for read-only device");
  return FALSE;
}

typedef struct {
  const char *key;
  const GVariantType *type;
  gboolean (* filter) (const char *key, GVariant *value, GUdevDevice *device, GError **error);
} XdpUsbAccessOptionKey;

static gboolean
filter_access_devices (XdpUsb         *self,
                       UsbSenderInfo  *sender_info,
                       GVariant       *devices,
                       GVariant      **out_filtered_devices,
                       GError        **out_error)
{
  g_auto(GVariantBuilder) filtered_devices_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(sa{sv}a{sv})"));
  GVariantIter *device_options_iter;
  GVariantIter devices_iter;
  const char *device_id;
  size_t n_devices;

  static const XdpUsbAccessOptionKey usb_device_options[] = {
    { "writable", G_VARIANT_TYPE_BOOLEAN, filter_access_devices_writable },
  };

  g_assert (self != NULL);
  g_assert (sender_info != NULL);
  g_assert (devices != NULL);
  g_assert (out_filtered_devices != NULL && *out_filtered_devices == NULL);
  g_assert (out_error != NULL && *out_error == NULL);

  n_devices = g_variant_iter_init (&devices_iter, devices);

  if (n_devices == 0)
    {
      g_set_error (out_error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "No devices in the devices array");
      return FALSE;
    }

  while (g_variant_iter_next (&devices_iter,
                              "(&sa{sv})",
                              &device_id,
                              &device_options_iter))
    {
      g_autoptr(GVariantIter) owned_deviced_options_iter = device_options_iter;
      g_autoptr(GVariant) device_variant = NULL;
      g_auto(GVariantDict) device_options_dict = G_VARIANT_DICT_INIT (NULL);
      GUdevDevice *device;
      GVariant *device_option_value;
      const char *device_option;

      device = g_hash_table_lookup (self->ids_to_devices, device_id);

      if (!device)
        {
          g_set_error (out_error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Device %s not available",
                       device_id);
          return FALSE;
        }

      g_assert (G_UDEV_IS_DEVICE (device));
      g_assert (g_strcmp0 (g_udev_device_get_subsystem (device), "usb") == 0);

      /* Can the app even request this device? */
      if (!usb_sender_info_match_device (sender_info, device))
        {
          g_set_error (out_error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                       "Access to device %s is not allowed",
                       device_id);
          return FALSE;
        }

      while (g_variant_iter_next (device_options_iter,
                                  "{&sv}",
                                  &device_option,
                                  &device_option_value))
        {
          g_autoptr(GVariant) value = device_option_value;

          for (size_t i = 0; i < G_N_ELEMENTS (usb_device_options); i++)
            {
              if (g_strcmp0 (device_option, usb_device_options[i].key) != 0)
                continue;

              if (!g_variant_is_of_type (value, usb_device_options[i].type))
                {
                  g_set_error (out_error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                               "Invalid type for option '%s'",
                               device_option);
                  return FALSE;
                }

              if (usb_device_options[i].filter &&
                  !usb_device_options[i].filter (device_option, value,
                                                 device, out_error))
                return FALSE;

              g_variant_dict_insert_value (&device_options_dict,
                                           device_option,
                                           value);
            }
        }

      device_variant = gudev_device_to_variant (self, sender_info, device);

      g_variant_builder_add (&filtered_devices_builder,
                             "(s@a{sv}@a{sv})",
                             device_id,
                             device_variant,
                             g_variant_dict_end (&device_options_dict));
    }

  *out_filtered_devices =
    g_variant_ref_sink (g_variant_builder_end (&filtered_devices_builder));
  return TRUE;
}

static gboolean
handle_acquire_devices (XdpDbusUsb            *object,
                        GDBusMethodInvocation *invocation,
                        const char            *arg_parent_window,
                        GVariant              *arg_devices,
                        GVariant              *arg_options)
{
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(UsbSenderInfo) sender_info = NULL;
  g_autoptr(GVariant) filtered_devices = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  XdpPermission permission;
  XdpRequest *request;
  XdpUsb *self;

  static const XdpOptionKey usb_acquire_devices_options[] = {
  };

  self = XDP_USB (object);
  request = xdp_request_from_invocation (invocation);

  g_debug ("[usb] Handling AccessDevices");

  REQUEST_AUTOLOCK (request);

  permission = xdp_get_permission_sync (request->app_info,
                                        PERMISSION_TABLE,
                                        PERMISSION_ID);
  if (permission == XDP_PERMISSION_NO)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed to create USB sessions");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (usb_impl)),
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       g_dbus_proxy_get_name (G_DBUS_PROXY (usb_impl)),
                                                       request->id,
                                                       NULL,
                                                       &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options,
                           &options_builder,
                           usb_acquire_devices_options,
                           G_N_ELEMENTS (usb_acquire_devices_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  sender_info = usb_sender_info_from_request (self, request);
  g_assert (sender_info != NULL);

  /* Validate devices */
  if (!filter_access_devices (self, sender_info, arg_devices, &filtered_devices, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_impl_usb_call_acquire_devices (usb_impl,
                                          request->id,
                                          arg_parent_window,
                                          xdp_app_info_get_id (request->app_info),
                                          filtered_devices,
                                          options,
                                          NULL,
                                          usb_acquire_devices_cb,
                                          g_object_ref (request));

  xdp_dbus_usb_complete_acquire_devices (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_finish_acquire_devices (XdpDbusUsb            *object,
                               GDBusMethodInvocation *invocation,
                               const char            *object_path,
                               GVariant              *arg_options)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  g_autoptr(UsbSenderInfo) sender_info = NULL;
  g_autoptr(GUnixFDList) fds = NULL;
  GVariantBuilder results_builder;
  XdpPermission permission;
  uint32_t accessed_devices;
  gboolean finished;
  GPtrArray *pending_devices = NULL;
  XdpUsb *self;

  self = XDP_USB (object);

  g_debug ("[usb] Handling FinishAccessDevices");

  sender_info = usb_sender_info_from_app_info (self, app_info);

  pending_devices = g_hash_table_lookup (sender_info->pending_devices, object_path);
  if (pending_devices == NULL)
    {
      /* We don't have the request in the pending_devices. Either it has
       * been cancelled or it simply doesn't exist.
       */
      g_hash_table_remove (sender_info->pending_devices, object_path);

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "There is no device being acquired");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  permission = xdp_get_permission_sync (app_info,
                                        PERMISSION_TABLE,
                                        PERMISSION_ID);
  if (permission == XDP_PERMISSION_NO)
    {
      /* If permission was revoked in between D-Bus calls, reset state */
      g_hash_table_remove (sender_info->pending_devices, object_path);

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed to access USB devices");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* We should never trigger these asserts. */
  g_assert (pending_devices != NULL);

  fds = g_unix_fd_list_new ();

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE ("a(sa{sv})"));

  accessed_devices = 0;
  while (accessed_devices < MAX_DEVICES &&
         pending_devices->len > 0)
    {
      g_autoptr(UsbDeviceAcquireData) access_data = NULL;
      g_autoptr(GError) error = NULL;
      GVariantDict dict;
      UsbOwnedDevice *owned_device = NULL;
      g_autofd int fd = -1;
      int fd_index;

      g_variant_dict_init (&dict, NULL);

      access_data = g_ptr_array_steal_index (pending_devices, 0);

      /* Check we haven't already acquired the device */
      owned_device = g_hash_table_lookup (sender_info->owned_devices, access_data->device_id);
      if (!owned_device)
        {
          const char *device_file;
          GUdevDevice *device;

          device = g_hash_table_lookup (self->ids_to_devices, access_data->device_id);

          if (!device)
            {
              g_variant_dict_insert (&dict, "success", "b", FALSE);
              g_variant_dict_insert (&dict, "error", "s", _("Device not available"));
              g_variant_builder_add (&results_builder, "(s@a{sv})",
                                     access_data->device_id,
                                 g_variant_dict_end (&dict));
              continue;
            }

          device_file = g_udev_device_get_device_file (device);
          g_assert (device_file != NULL);

          /* Can the app even request this device? */
          if (!usb_sender_info_match_device (sender_info, device))
            {
              g_variant_dict_insert (&dict, "success", "b", FALSE);
              g_variant_dict_insert (&dict, "error", "s", _("Not allowed"));
              g_variant_builder_add (&results_builder, "(s@a{sv})",
                                     access_data->device_id,
                                     g_variant_dict_end (&dict));
              continue;
            }

          fd = open (device_file, access_data->writable ? O_RDWR : O_RDONLY);
          if (fd == -1)
            {
              g_variant_dict_insert (&dict, "success", "b", FALSE);
              g_variant_dict_insert (&dict, "error", "s", g_strerror (errno));
              g_variant_builder_add (&results_builder, "(s@a{sv})",
                                     access_data->device_id,
                                     g_variant_dict_end (&dict));
              continue;
            }
          fd_index = g_unix_fd_list_append (fds, fd, &error);
        }
      else
        {
	  /* If we have already acquired the device, just return the fd again */
	  fd_index = g_unix_fd_list_append (fds, owned_device->fd, &error);
	}

      if (error)
        {
          g_variant_dict_insert (&dict, "success", "b", FALSE);
          g_variant_dict_insert (&dict, "error", "s", error->message);
          g_variant_builder_add (&results_builder, "(s@a{sv})",
                                 access_data->device_id,
                                 g_variant_dict_end (&dict));
          continue;
        }

      /* This sender now owns this device. Either create a new one
       * or ref the existing one.
       */
      if (!owned_device)
        {
          usb_sender_info_acquire_device (sender_info,
                                          access_data->device_id,
                                          g_steal_fd (&fd));
        }
      else
        {
          usb_owned_device_ref (owned_device);
        }

      g_variant_dict_insert (&dict, "success", "b", TRUE);
      g_variant_dict_insert (&dict, "fd", "h", fd_index);
      g_variant_builder_add (&results_builder, "(s@a{sv})",
                             access_data->device_id,
                             g_variant_dict_end (&dict));

      accessed_devices++;
    }

  finished = pending_devices->len == 0;

  if (finished)
    {
      g_hash_table_remove (sender_info->pending_devices, object_path);
    }

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           g_variant_new ("(@a(sa{sv})b)",
                                                                          g_variant_builder_end (&results_builder),
                                                                          finished),
                                                           fds);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_release_devices (XdpDbusUsb            *object,
                        GDBusMethodInvocation *invocation,
                        const char * const    *arg_devices,
                        GVariant              *arg_options)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(UsbSenderInfo) sender_info = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  XdpUsb *self;

  static const XdpOptionKey usb_release_devices_options[] = {
  };

  self = XDP_USB (object);

  g_debug ("[usb] Handling ReleaseDevices");

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options,
                           &options_builder,
                           usb_release_devices_options,
                           G_N_ELEMENTS (usb_release_devices_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  sender_info = usb_sender_info_from_app_info (self, app_info);
  g_assert (sender_info != NULL);

  for (size_t i = 0; arg_devices && arg_devices[i]; i++)
    usb_sender_info_release_device (sender_info, arg_devices[i]);

  xdp_dbus_usb_complete_release_devices (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_usb_iface_init (XdpDbusUsbIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_enumerate_devices = handle_enumerate_devices;
  iface->handle_acquire_devices = handle_acquire_devices;
  iface->handle_finish_acquire_devices = handle_finish_acquire_devices;
  iface->handle_release_devices = handle_release_devices;
}

static void
xdp_usb_dispose (GObject *object)
{
  XdpUsb *self = XDP_USB (object);

  g_clear_pointer (&self->ids_to_devices, g_hash_table_unref);
  g_clear_pointer (&self->syspaths_to_ids, g_hash_table_unref);
  g_clear_pointer (&self->sessions, g_hash_table_unref);
  g_clear_pointer (&self->sender_infos, g_hash_table_unref);

  g_clear_object (&self->gudev_client);
}

static void
xdp_usb_class_init (XdpUsbClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_usb_dispose;
}

static void
xdp_usb_init (XdpUsb *self)
{
  g_autolist(GUdevDevice) devices = NULL;
  static const char * const subsystems[] = {
    "usb",
    NULL,
  };

  g_debug ("[usb] Initializing USB portal");

  xdp_dbus_usb_set_version (XDP_DBUS_USB (self), 1);

  self->ids_to_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
  self->syspaths_to_ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_free);
  self->sessions = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->sender_infos =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) usb_sender_info_unref);

  self->gudev_client = g_udev_client_new (subsystems);
  g_signal_connect (self->gudev_client,
                    "uevent",
                    G_CALLBACK (gudev_client_uevent_cb),
                    self);

  /* Initialize devices */
  devices = g_udev_client_query_by_subsystem (self->gudev_client, "usb");
  for (GList *l = devices; l; l = l->next)
    {
      g_autofree char *id = NULL;
      GUdevDevice *device = l->data;

      if (!is_gudev_device_suitable (device))
        continue;

      id = register_with_unique_usb_id (self, device);
    }
}

void
xdp_usb_delete_for_sender (const char *sender)
{
  if (usb && g_hash_table_remove (usb->sender_infos, sender))
    g_debug ("Removed sender %s", sender);
}

void
init_usb (XdpContext *context)
{
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config,
                                        "org.freedesktop.impl.portal.Usb");
  if (impl_config == NULL)
    return;

  usb_impl = xdp_dbus_impl_usb_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               impl_config->dbus_name,
                                               DESKTOP_PORTAL_OBJECT_PATH,
                                               NULL,
                                               &error);
  if (usb_impl == NULL)
    {
      g_warning ("Failed to create USB proxy: %s", error->message);
      return;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (usb_impl), G_MAXINT);

  g_assert (usb_impl != NULL);
  g_assert (usb == NULL);

  usb = g_object_new (xdp_usb_get_type (), NULL);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (usb),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
