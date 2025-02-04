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

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-login.h>
#include "sd-escape.h"
#endif

#include "xdp-app-info-host-private.h"
#include "xdp-usb-query.h"

struct _XdpAppInfoHost
{
  XdpAppInfo parent;

  GPtrArray *usb_queries;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoHost, xdp_app_info_host, XDP_TYPE_APP_INFO)

static const GPtrArray *
xdp_app_info_host_get_usb_queries (XdpAppInfo *app_info)
{
  XdpAppInfoHost *app_info_host = XDP_APP_INFO_HOST (app_info);

  return app_info_host->usb_queries;
}

static gboolean
xdp_app_info_host_is_valid_sub_app_id (XdpAppInfo *app_info,
                                       const char *sub_app_id)
{
  return TRUE;
}

static gboolean
xdp_app_info_host_validate_autostart (XdpAppInfo          *app_info,
                                      GKeyFile            *keyfile,
                                      const char * const  *autostart_exec,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  return TRUE;
}

static gboolean
xdp_app_info_host_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                             GKeyFile    *key_file,
                                             GError     **error)
{
  return TRUE;
}

static void
xdp_app_info_host_dispose (GObject *object)
{
  XdpAppInfoHost *app_info = XDP_APP_INFO_HOST (object);

  g_clear_pointer (&app_info->usb_queries, g_ptr_array_unref);

  G_OBJECT_CLASS (xdp_app_info_host_parent_class)->dispose (object);
}

static void
xdp_app_info_host_class_init (XdpAppInfoHostClass *klass)
{
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_host_dispose;

  app_info_class->get_usb_queries =
    xdp_app_info_host_get_usb_queries;
  app_info_class->validate_autostart =
    xdp_app_info_host_validate_autostart;
  app_info_class->validate_dynamic_launcher =
    xdp_app_info_host_validate_dynamic_launcher;
  app_info_class->is_valid_sub_app_id =
    xdp_app_info_host_is_valid_sub_app_id;
}

static void
xdp_app_info_host_init (XdpAppInfoHost *app_info_host)
{
  g_autoptr(XdpUsbQuery) query = NULL;

  app_info_host->usb_queries =
    g_ptr_array_new_with_free_func ((GDestroyNotify) xdp_usb_query_free);

  query = xdp_usb_query_from_string (XDP_USB_QUERY_TYPE_ENUMERABLE, "all");
  if (query)
    g_ptr_array_add (app_info_host->usb_queries, g_steal_pointer (&query));
}

#ifdef HAVE_LIBSYSTEMD
char *
_xdp_app_info_host_parse_app_id_from_unit_name (const char *unit)
{
  g_autoptr(GRegex) regex1 = NULL;
  g_autoptr(GRegex) regex2 = NULL;
  g_autoptr(GMatchInfo) match = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *app_id = NULL;

  g_assert (g_str_has_prefix (unit, "app-"));

  /*
   * From https://systemd.io/DESKTOP_ENVIRONMENTS/ the format is one of:
   * app[-<launcher>]-<ApplicationID>-<RANDOM>.scope
   * app[-<launcher>]-<ApplicationID>-<RANDOM>.slice
   */
  regex1 = g_regex_new ("^app-(?:[[:alnum:]]+\\-)?(.+?)(?:\\-[[:alnum:]]*)(?:\\.scope|\\.slice)$", 0, 0, &error);
  g_assert (error == NULL);
  /*
   * app[-<launcher>]-<ApplicationID>-autostart.service -> no longer true since systemd v248
   * app[-<launcher>]-<ApplicationID>[@<RANDOM>].service
   */
  regex2 = g_regex_new ("^app-(?:[[:alnum:]]+\\-)?(.+?)(?:@[[:alnum:]]*|\\-autostart)?\\.service$", 0, 0, &error);
  g_assert (error == NULL);

  if (!g_regex_match (regex1, unit, 0, &match))
    g_clear_pointer (&match, g_match_info_unref);

  if (match == NULL && !g_regex_match (regex2, unit, 0, &match))
    g_clear_pointer (&match, g_match_info_unref);

  if (match != NULL)
    {
      g_autofree char *escaped_app_id = NULL;
      /* Unescape the unit name which may have \x hex codes in it, e.g.
       * "app-gnome-org.gnome.Evolution\x2dalarm\x2dnotify-2437.scope"
       */
      escaped_app_id = g_match_info_fetch (match, 1);
      if (cunescape (escaped_app_id, UNESCAPE_RELAX, &app_id) < 0)
        app_id = g_strdup ("");
    }
  else
    {
      app_id = g_strdup ("");
    }

  return g_steal_pointer (&app_id);
}
#endif /* HAVE_LIBSYSTEMD */

static char *
get_appid_from_pid (pid_t pid)
{
#ifdef HAVE_LIBSYSTEMD
  g_autofree char *unit = NULL;
  int res;

  res = sd_pid_get_user_unit (pid, &unit);
  /*
   * The session might not be managed by systemd or there could be an error
   * fetching our own systemd units or the unit might not be started by the
   * desktop environment (e.g. it's a script run from terminal).
   */
  if (res == -ENODATA || res < 0 || !unit || !g_str_has_prefix (unit, "app-"))
    return g_strdup ("");

  return _xdp_app_info_host_parse_app_id_from_unit_name (unit);

#else
  /* FIXME: we should return NULL and handle id==NULL at callers */
  return g_strdup ("");
#endif /* HAVE_LIBSYSTEMD */
}

XdpAppInfo *
xdp_app_info_host_new_registered (int          pidfd,
                                  const char  *app_id,
                                  GError     **error)
{
  g_autoptr(XdpAppInfoHost) app_info_host = NULL;

  app_info_host = g_initable_new (XDP_TYPE_APP_INFO_HOST,
                                  NULL,
                                  error,
                                  "engine", NULL,
                                  "id", app_id,
                                  "pidfd", pidfd,
                                  "flags", XDP_APP_INFO_FLAG_HAS_NETWORK |
                                           XDP_APP_INFO_FLAG_SUPPORTS_OPATH |
                                           XDP_APP_INFO_FLAG_REQUIRE_GAPPINFO,
                                  NULL);

  return XDP_APP_INFO (g_steal_pointer (&app_info_host));
}

XdpAppInfo *
xdp_app_info_host_new (int pid,
                       int pidfd)
{
  g_autoptr(XdpAppInfoHost) app_info_host = NULL;
  g_autofree char *app_id = NULL;

  app_id = get_appid_from_pid (pid);

  app_info_host = g_initable_new (XDP_TYPE_APP_INFO_HOST,
                                  NULL,
                                  NULL,
                                  "engine", NULL,
                                  "id", app_id,
                                  "pidfd", pidfd,
                                  "flags", XDP_APP_INFO_FLAG_HAS_NETWORK |
                                           XDP_APP_INFO_FLAG_SUPPORTS_OPATH,
                                  NULL);

  return XDP_APP_INFO (g_steal_pointer (&app_info_host));
}
