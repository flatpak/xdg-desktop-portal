/*
 * Copyright © 2023-2024 GNOME Foundation Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
 *       Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *       Hubert Figuière <hub@figuiere.net>
 */

#include <stdint.h>

#pragma once

typedef enum
{
  XDP_USB_RULE_TYPE_ALL,
  XDP_USB_RULE_TYPE_CLASS,
  XDP_USB_RULE_TYPE_DEVICE,
  XDP_USB_RULE_TYPE_VENDOR,
} UsbRuleType;

typedef enum
{
  XDP_USB_RULE_CLASS_TYPE_CLASS_ONLY,
  XDP_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS,
} UsbDeviceClassType;

typedef struct
{
  UsbDeviceClassType type;
  uint16_t class;
  uint16_t subclass;
} UsbDeviceClass;

typedef struct
{
  uint16_t id;
} UsbProduct;

typedef struct
{
  uint16_t id;
} UsbVendor;

typedef struct
{
  UsbRuleType rule_type;

  union {
    UsbDeviceClass device_class;
    UsbProduct product;
    UsbVendor vendor;
  } d;
} XdpUsbRule;

typedef enum
{
  XDP_USB_QUERY_TYPE_HIDDEN,
  XDP_USB_QUERY_TYPE_ENUMERABLE,
} XdpUsbQueryType;

typedef struct
{
  XdpUsbQueryType query_type;
  GPtrArray *rules;
} XdpUsbQuery;

void xdp_usb_query_free (XdpUsbQuery *query);
XdpUsbQuery *xdp_usb_query_from_string (XdpUsbQueryType  query_type,
					const char      *string);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpUsbQuery, xdp_usb_query_free);

gboolean
xdp_validate_hex_uint16 (const char *value,
                         size_t      expected_length,
                         uint16_t   *out_value);
