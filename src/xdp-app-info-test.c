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

static GInitableIface *initable_parent_iface;

static void initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpAppInfoTest,
                               xdp_app_info_test,
                               XDP_TYPE_APP_INFO,
                               G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                      initable_iface_init))

static GPtrArray * parse_usb_queries_string (const char *usb_queries_str);

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

static gboolean
app_info_test_initable_init (GInitable     *initable,
                             GCancellable  *cancellable,
                             GError       **error)
{
  XdpAppInfoTest *app_info_test = XDP_APP_INFO_TEST (initable);
  const char *app_id;
  const char *usb_queries;
  const char *registered;
  g_autofree char *desktop_id = NULL;
  g_autoptr(GAppInfo) gappinfo = NULL;

  app_id = g_getenv ("XDG_DESKTOP_PORTAL_TEST_APP_ID");
  if (!app_id)
    {
      g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                   "Env XDG_DESKTOP_PORTAL_TEST_APP_ID is not set");
      return FALSE;
    }

  registered = xdp_app_info_get_registered (XDP_APP_INFO (app_info_test));
  if (registered)
    app_id = registered;

  desktop_id = g_strconcat (app_id, ".desktop", NULL);
  gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  xdp_app_info_set_identity (XDP_APP_INFO (app_info_test),
                             "",
                             app_id,
                             NULL);
  xdp_app_info_set_gappinfo (XDP_APP_INFO (app_info_test), gappinfo);
  xdp_app_info_set_flags (XDP_APP_INFO (app_info_test),
                          XDP_APP_INFO_FLAG_HAS_NETWORK |
                          XDP_APP_INFO_FLAG_SUPPORTS_OPATH);

  usb_queries = g_getenv ("XDG_DESKTOP_PORTAL_TEST_USB_QUERIES");

  app_info_test->usb_queries = parse_usb_queries_string (usb_queries);

  return initable_parent_iface->init (initable, cancellable, error);
}

static void
xdp_app_info_test_init (XdpAppInfoTest *app_info_test)
{
}

static void
initable_iface_init (GInitableIface *iface)
{
  initable_parent_iface = g_type_interface_peek_parent (iface);

  iface->init = app_info_test_initable_init;
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
xdp_app_info_test_new (int         pid,
                       int         pidfd,
                       const char *registered)
{
  return g_initable_new (XDP_TYPE_APP_INFO_TEST,
                         NULL,
                         NULL,
                         "pid", pid,
                         "pidfd", pidfd,
                         "registered", registered,
                         NULL);
}
