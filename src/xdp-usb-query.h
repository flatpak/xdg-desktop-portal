/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include <stdint.h>

#include <glib.h>

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
