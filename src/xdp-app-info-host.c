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

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-login.h>
#include "sd-escape.h"
#endif

#include "xdp-app-info-host-private.h"

struct _XdpAppInfoHost
{
  XdpAppInfo parent;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoHost, xdp_app_info_host, XDP_TYPE_APP_INFO)

gboolean
xdp_app_info_host_validate_autostart (XdpAppInfo          *app_info,
                                      GKeyFile            *keyfile,
                                      const char * const  *autostart_exec,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  return TRUE;
}

gboolean
xdp_app_info_host_validate_dynamic_launcher (XdpAppInfo  *app_info,
                                             GKeyFile    *key_file,
                                             GError     **error)
{
  return TRUE;
}


static void
xdp_app_info_host_class_init (XdpAppInfoHostClass *klass)
{
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);

  app_info_class->validate_autostart =
    xdp_app_info_host_validate_autostart;
  app_info_class->validate_dynamic_launcher =
    xdp_app_info_host_validate_dynamic_launcher;
}

static void
xdp_app_info_host_init (XdpAppInfoHost *app_info_host)
{
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
xdp_app_info_host_new (int pid,
                       int pidfd)
{
  g_autoptr (XdpAppInfoHost) app_info_host = NULL;
  g_autofree char *appid = NULL;
  g_autofree char *desktop_id = NULL;
  g_autoptr(GAppInfo) gappinfo = NULL;

  appid = get_appid_from_pid (pid);

  desktop_id = g_strconcat (appid, ".desktop", NULL);
  gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  app_info_host = g_object_new (XDP_TYPE_APP_INFO_HOST, NULL);
  xdp_app_info_initialize (XDP_APP_INFO (app_info_host),
                            /* engine, app id, instance */
                           NULL, appid, NULL,
                           pidfd, gappinfo,
                           /* supports_opath */ TRUE,
                           /* has_network */ TRUE,
                           /* requires_pid_mapping */ FALSE);

  return XDP_APP_INFO (g_steal_pointer (&app_info_host));
}
