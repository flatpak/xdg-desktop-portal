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
#include <glib.h>

#include "xdp-usb-query.h"

static void
xdp_usb_rule_free (XdpUsbRule *rule)
{
  g_free (rule);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpUsbRule, xdp_usb_rule_free);

gboolean
xdp_validate_hex_uint16 (const char *value,
                         size_t      expected_length,
                         uint16_t   *out_value)
{
  size_t len;
  char *end;
  long n;

  g_assert (value != NULL);
  g_assert (expected_length > 0 && expected_length <= 4);

  len = strlen (value);
  if (len != expected_length)
    return FALSE;

  n = strtol (value, &end, 16);

  if (end - value != len)
    return FALSE;

  if (n <= 0 || n > UINT16_MAX)
    return FALSE;

  if (out_value)
    *out_value = n;

  return TRUE;
}

static gboolean
parse_all_usb_rule (XdpUsbRule  *dest,
                    GStrv        data)
{
  if (g_strv_length (data) != 1)
    return FALSE;

  dest->rule_type = XDP_USB_RULE_TYPE_ALL;
  return TRUE;
}

static gboolean
parse_cls_usb_rule (XdpUsbRule  *dest,
                    GStrv        data)
{
  const char *subclass;
  const char *class;

  if (g_strv_length (data) < 3)
    return FALSE;

  class = data[1];
  subclass = data[2];

  if (!xdp_validate_hex_uint16 (class, 2, &dest->d.device_class.class))
    return FALSE;

  if (g_strcmp0 (subclass, "*") == 0)
    dest->d.device_class.type = XDP_USB_RULE_CLASS_TYPE_CLASS_ONLY;
  else if (xdp_validate_hex_uint16 (subclass, 2, &dest->d.device_class.subclass))
    dest->d.device_class.type = XDP_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS;
  else
    return FALSE;

  dest->rule_type = XDP_USB_RULE_TYPE_CLASS;
  return TRUE;
}

static gboolean
parse_dev_usb_rule (XdpUsbRule *dest,
                    GStrv       data)
{
  if (g_strv_length (data) != 2)
    return FALSE;

  if (!xdp_validate_hex_uint16 (data[1], 4, &dest->d.product.id))
    return FALSE;

  dest->rule_type = XDP_USB_RULE_TYPE_DEVICE;
  return TRUE;
}

static gboolean
parse_vnd_usb_rule (XdpUsbRule *dest,
                    GStrv       data)
{
  if (g_strv_length (data) != 2)
    return FALSE;

  if (!xdp_validate_hex_uint16 (data[1], 4, &dest->d.product.id))
    return FALSE;

  dest->rule_type = XDP_USB_RULE_TYPE_VENDOR;
  return TRUE;
}

static const struct {
  const char *name;
  gboolean (*parse) (XdpUsbRule *dest,
                     GStrv       data);
} rule_parsers[] = {
  { "all", parse_all_usb_rule },
  { "cls", parse_cls_usb_rule },
  { "dev", parse_dev_usb_rule },
  { "vnd", parse_vnd_usb_rule },
};

static XdpUsbRule *
xdp_usb_rule_from_string (const char *string)
{
  g_autoptr(XdpUsbRule) usb_rule = NULL;
  g_auto(GStrv) split = NULL;
  gboolean parsed = FALSE;

  split = g_strsplit (string, ":", 0);

  if (!split || g_strv_length (split) > 3)
    return NULL;

  usb_rule = g_new0 (XdpUsbRule, 1);

  for (size_t i = 0; i < G_N_ELEMENTS (rule_parsers); i++)
    {
      if (g_strcmp0 (rule_parsers[i].name, split[0]) == 0)
        {
          if (!rule_parsers[i].parse (usb_rule, split))
            return FALSE;

          parsed = TRUE;
          break;
        }
    }

  if (!parsed)
    return NULL;

  return g_steal_pointer (&usb_rule);
}

void
xdp_usb_query_free (XdpUsbQuery *query)
{
  g_return_if_fail (query != NULL);

  g_clear_pointer (&query->rules, g_ptr_array_unref);
  g_free (query);
}

XdpUsbQuery *
xdp_usb_query_from_string (XdpUsbQueryType  query_type,
                           const char      *string)
{
  g_autoptr(XdpUsbQuery) usb_query = NULL;
  g_auto(GStrv) split = NULL;

  split = g_strsplit (string, "+", 0);
  if (!split)
    return NULL;

  usb_query = g_new0 (XdpUsbQuery, 1);
  usb_query->query_type = query_type;
  usb_query->rules = g_ptr_array_new_with_free_func ((GDestroyNotify) xdp_usb_rule_free);

  for (size_t i = 0; split[i] != NULL; i++)
    {
      g_autoptr(XdpUsbRule) usb_rule = NULL;
      const char *rule = split[i];

      usb_rule = xdp_usb_rule_from_string (rule);
      if (!usb_rule)
        return NULL;

      g_ptr_array_add (usb_query->rules, g_steal_pointer (&usb_rule));
    }

  g_return_val_if_fail (usb_query->rules->len > 0, NULL);

  return g_steal_pointer (&usb_query);
}
