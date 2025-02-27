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

enum
{
  PROP_0,
  PROP_REGISTERED,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

struct _XdpAppInfoHost
{
  XdpAppInfo parent;

  char *registered;
  GPtrArray *usb_queries;
};

static GInitableIface *initable_parent_iface;

static void initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpAppInfoHost,
                               xdp_app_info_host,
                               XDP_TYPE_APP_INFO,
                               G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                      initable_iface_init))

static char * get_appid_from_pid (pid_t pid);

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
xdp_app_info_host_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  XdpAppInfoHost *app_info = XDP_APP_INFO_HOST (object);

  switch (prop_id)
    {
    case PROP_REGISTERED:
      g_value_set_string (value, app_info->registered);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
xdp_app_info_host_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  XdpAppInfoHost *app_info = XDP_APP_INFO_HOST (object);

  switch (prop_id)
    {
    case PROP_REGISTERED:
      g_assert (app_info->registered == NULL);
      app_info->registered = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
xdp_app_info_host_dispose (GObject *object)
{
  XdpAppInfoHost *app_info = XDP_APP_INFO_HOST (object);

  g_clear_pointer (&app_info->usb_queries, g_ptr_array_unref);
  g_clear_pointer (&app_info->registered, g_free);

  G_OBJECT_CLASS (xdp_app_info_host_parent_class)->dispose (object);
}

static gboolean
app_info_host_initable_init (GInitable     *initable,
                             GCancellable  *cancellable,
                             GError       **error)
{
  XdpAppInfoHost *app_info_host = XDP_APP_INFO_HOST (initable);
  gboolean is_testing;
  g_autofree char *owned_app_id = NULL;
  const char *app_id = NULL;
  g_autofree char *desktop_id = NULL;
  g_autoptr(GAppInfo) gappinfo = NULL;
  g_autoptr(XdpUsbQuery) query = NULL;

  is_testing = xdp_app_info_is_testing (XDP_APP_INFO (app_info_host));

  if (app_info_host->registered)
    {
      app_id = app_info_host->registered;
    }
  else if (is_testing)
    {
      app_id = g_getenv ("XDG_DESKTOP_PORTAL_TEST_APP_ID");
      g_assert (app_id != NULL);
    }
  else
    {
      int pid = xdp_app_info_get_pid (XDP_APP_INFO (app_info_host));

      owned_app_id = get_appid_from_pid (pid);
      app_id = owned_app_id;
    }

  desktop_id = g_strconcat (app_id, ".desktop", NULL);
  gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  xdp_app_info_set_identity (XDP_APP_INFO (app_info_host), NULL, app_id, NULL);
  xdp_app_info_set_gappinfo (XDP_APP_INFO (app_info_host), gappinfo);
  xdp_app_info_set_flags (XDP_APP_INFO (app_info_host),
                          XDP_APP_INFO_FLAG_HAS_NETWORK |
                          XDP_APP_INFO_FLAG_SUPPORTS_OPATH);

  app_info_host->usb_queries =
    g_ptr_array_new_with_free_func ((GDestroyNotify) xdp_usb_query_free);

  query = xdp_usb_query_from_string (XDP_USB_QUERY_TYPE_ENUMERABLE, "all");
  if (query)
    g_ptr_array_add (app_info_host->usb_queries, g_steal_pointer (&query));

  return initable_parent_iface->init (initable, cancellable, error);
}

static void
xdp_app_info_host_init (XdpAppInfoHost *app_info_host)
{
}

static void
initable_iface_init (GInitableIface *iface)
{
  initable_parent_iface = g_type_interface_peek_parent (iface);

  iface->init = app_info_host_initable_init;
}

static void
xdp_app_info_host_class_init (XdpAppInfoHostClass *klass)
{
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_host_dispose;
  object_class->get_property = xdp_app_info_host_get_property;
  object_class->set_property = xdp_app_info_host_set_property;

  app_info_class->get_usb_queries =
    xdp_app_info_host_get_usb_queries;
  app_info_class->validate_autostart =
    xdp_app_info_host_validate_autostart;
  app_info_class->validate_dynamic_launcher =
    xdp_app_info_host_validate_dynamic_launcher;
  app_info_class->is_valid_sub_app_id =
    xdp_app_info_host_is_valid_sub_app_id;

  properties[PROP_REGISTERED] =
    g_param_spec_string ("registered", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
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
