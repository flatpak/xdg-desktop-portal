/*
 * Copyright © 2024 Red Hat, Inc
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "xdp-app-info-test-private.h"

#include "xdp-usb-query.h"

struct _XdpAppInfoTest
{
  XdpAppInfo parent;

  GPtrArray *usb_queries;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoTest, xdp_app_info_test, XDP_TYPE_APP_INFO)

static gboolean
xdp_app_info_test_validate_autostart (XdpAppInfo          *app_info,
                                      GKeyFile            *keyfile,
                                      const char * const  *autostart_exec,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  return TRUE;
}

gboolean
xdp_app_info_test_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                             GKeyFile    *key_file,
                                             GError     **error)
{
  return TRUE;
}

static const GPtrArray *
xdp_app_info_test_get_usb_queries (XdpAppInfo *app_info)
{
  XdpAppInfoTest *app_info_test = XDP_APP_INFO_TEST (app_info);

  return app_info_test->usb_queries;
}

static gboolean
xdp_app_info_test_is_valid_sub_app_id (XdpAppInfo *app_info,
                                       const char *sub_app_id)
{
  return TRUE;
}

static void
xdp_app_info_test_dispose (GObject *object)
{
  XdpAppInfoTest *app_info = XDP_APP_INFO_TEST (object);

  g_clear_pointer (&app_info->usb_queries, g_ptr_array_unref);

  G_OBJECT_CLASS (xdp_app_info_test_parent_class)->dispose (object);
}

static void
xdp_app_info_test_class_init (XdpAppInfoTestClass *klass)
{
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_test_dispose;

  app_info_class->validate_autostart =
    xdp_app_info_test_validate_autostart;
  app_info_class->validate_dynamic_launcher =
    xdp_app_info_test_validate_dynamic_launcher;
  app_info_class->get_usb_queries =
    xdp_app_info_test_get_usb_queries;
  app_info_class->is_valid_sub_app_id =
    xdp_app_info_test_is_valid_sub_app_id;
}

static void
xdp_app_info_test_init (XdpAppInfoTest *app_info_test)
{
}

static GPtrArray *
parse_usb_queries_string (const char *usb_queries_str)
{
  g_autoptr(GPtrArray) usb_queries = NULL;
  g_auto(GStrv) queries_strs = NULL;

  if (!usb_queries_str)
    return NULL;

  usb_queries =
    g_ptr_array_new_with_free_func ((GDestroyNotify) xdp_usb_query_free);

  queries_strs = g_strsplit (usb_queries_str, ";", 0);
  for (size_t i = 0; queries_strs[i] != NULL; i++)
    {
      g_autoptr(XdpUsbQuery) query =
        xdp_usb_query_from_string (XDP_USB_QUERY_TYPE_ENUMERABLE,
                                   queries_strs[i]);

      if (query)
        g_ptr_array_add (usb_queries, g_steal_pointer (&query));
    }

  if (usb_queries->len == 0)
    return NULL;

  return g_steal_pointer (&usb_queries);
}

XdpAppInfo *
xdp_app_info_test_new (const char *app_id,
                       const char *usb_queries_str)
{
  g_autoptr (XdpAppInfoTest) app_info_test = NULL;

  app_info_test = g_object_new (XDP_TYPE_APP_INFO_TEST, NULL);
  xdp_app_info_initialize (XDP_APP_INFO (app_info_test),
                           "", app_id, NULL,
                           -1, NULL,
                           XDP_APP_INFO_FLAG_HAS_NETWORK |
                           XDP_APP_INFO_FLAG_SUPPORTS_OPATH);

  app_info_test->usb_queries = parse_usb_queries_string (usb_queries_str);

  return XDP_APP_INFO (g_steal_pointer (&app_info_test));
}
