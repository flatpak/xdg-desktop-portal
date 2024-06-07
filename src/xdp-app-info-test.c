/*
 * Copyright © 2024 Red Hat, Inc
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
 */

#include "config.h"

#include "xdp-app-info-test-private.h"

struct _XdpAppInfoTest
{
  XdpAppInfo parent;
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

static void
xdp_app_info_test_class_init (XdpAppInfoTestClass *klass)
{
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);

  app_info_class->validate_autostart =
    xdp_app_info_test_validate_autostart;
  app_info_class->validate_dynamic_launcher =
    xdp_app_info_test_validate_dynamic_launcher;
}

static void
xdp_app_info_test_init (XdpAppInfoTest *app_info_test)
{
}

XdpAppInfo *
xdp_app_info_test_new (const char *app_id)
{
  g_autoptr (XdpAppInfoTest) app_info_test = NULL;

  app_info_test = g_object_new (XDP_TYPE_APP_INFO_TEST, NULL);
  xdp_app_info_initialize (XDP_APP_INFO (app_info_test),
                           "", app_id, NULL,
                           -1, NULL,
                           TRUE, TRUE, TRUE);

  return XDP_APP_INFO (g_steal_pointer (&app_info_test));
}
