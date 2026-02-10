/*
 * Copyright © 2019 Red Hat, Inc
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Felipe Borges <feborges@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-request-future.h"
#include "xdp-utils.h"

#include "wallpaper.h"

struct _XdpWallpaper
{
  XdpDbusWallpaperSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplWallpaper *impl;
  XdpDbusImplAccess *access_impl;
};

#define XDP_TYPE_WALLPAPER (xdp_wallpaper_get_type ())
G_DECLARE_FINAL_TYPE (XdpWallpaper,
                      xdp_wallpaper,
                      XDP, WALLPAPER,
                      XdpDbusWallpaperSkeleton)

static void xdp_wallpaper_iface_init (XdpDbusWallpaperIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpWallpaper,
                               xdp_wallpaper,
                               XDP_DBUS_TYPE_WALLPAPER_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WALLPAPER,
                                                      xdp_wallpaper_iface_init));

static gboolean
validate_set_on (const char  *key,
                 GVariant    *value,
                 GVariant    *options,
                 gpointer     user_data,
                 GError     **error)
{
  const char *string = g_variant_get_string (value, NULL);

  return ((g_strcmp0 (string, "both") == 0) ||
          (g_strcmp0 (string, "background") == 0) ||
          (g_strcmp0 (string, "lockscreen") == 0));
}

static XdpOptionKey wallpaper_options[] = {
  { "show-preview", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "set-on", G_VARIANT_TYPE_STRING, validate_set_on }
};

static gboolean
get_permission (XdpWallpaper      *wallpaper,
                XdpRequestFuture  *request,
                XdpAppInfo        *app_info,
                GVariant          *arg_options,
                const char        *arg_parent_window,
                gboolean          *permission_granted_out,
                GError           **error)
{
  XdpPermission permission;
  gboolean show_preview = FALSE;
  g_autoptr(XdpDbusImplAccessAccessDialogResult) result = NULL;
  g_autoptr(GError) local_error = NULL;

  permission = dex_await_uint (xdp_permission_get_future (app_info,
                                                          WALLPAPER_PERMISSION_TABLE,
                                                          WALLPAPER_PERMISSION_ID),
                              &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (permission == XDP_PERMISSION_NO)
    {
      *permission_granted_out = FALSE;
      return TRUE;
    }

  g_variant_lookup (arg_options, "show-preview", "b", &show_preview);
  if (permission == XDP_PERMISSION_YES || show_preview)
    {
      *permission_granted_out = TRUE;
      return TRUE;
    }

  {
    const char *app_name = xdp_app_info_get_app_display_name (app_info);
    g_auto(GVariantBuilder) access_opt_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
    g_autofree gchar *title = NULL;
    g_autofree gchar *subtitle = NULL;
    const gchar *body;

    g_variant_builder_add (&access_opt_builder, "{sv}",
                           "deny_label", g_variant_new_string (_("Deny")));
    g_variant_builder_add (&access_opt_builder, "{sv}",
                           "grant_label", g_variant_new_string (_("Allow")));
    g_variant_builder_add (&access_opt_builder, "{sv}",
                           "icon", g_variant_new_string ("preferences-desktop-wallpaper-symbolic"));

    if (app_name)
      {
        title = g_strdup_printf (_("Allow %s to Set Backgrounds?"), app_name);
        subtitle = g_strdup_printf (_("%s wants to change the background image"),
                                    app_name);
      }
    else
      {
        title = g_strdup (_("Allow Apps to Set Backgrounds?"));
        subtitle = g_strdup (_("An app wants to change the background image"));
      }

    body = _("This permission can be changed at any time from the privacy settings");

    result = dex_await_boxed (xdp_dbus_impl_access_call_access_dialog_future (
        wallpaper->access_impl,
        xdp_request_future_get_object_path (request),
        xdp_app_info_get_id (app_info),
        arg_parent_window,
        title,
        subtitle,
        body,
        g_variant_builder_end (&access_opt_builder)),
      error);
    if (!result)
      return FALSE;
  }

  if (permission == XDP_PERMISSION_UNSET)
    {
      if (!dex_await_boolean (xdp_permission_set_future (app_info,
                                                         WALLPAPER_PERMISSION_TABLE,
                                                         WALLPAPER_PERMISSION_ID,
                                                         result->response == 0 ?
                                                         XDP_PERMISSION_YES : XDP_PERMISSION_NO),
                              &local_error))
        g_warning ("Setting unset permission failed: %s", local_error->message);
    }

  *permission_granted_out = (result->response != 0);
  return TRUE;
}

static gboolean
handle_set_wallpaper_uri (XdpDbusWallpaper      *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_parent_window,
                          const char            *arg_uri,
                          GVariant              *arg_options)
{
  XdpWallpaper *wallpaper = XDP_WALLPAPER (object);
  g_autoptr(XdpRequestFuture) request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  gboolean permission_granted;
  g_autoptr(GError) error = NULL;

  if (!xdp_filter_options (arg_options,
                           &opt_builder,
                           wallpaper_options,
                           G_N_ELEMENTS (wallpaper_options),
                           NULL,
                           &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_future_new (wallpaper->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (wallpaper->impl),
                                                      arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_wallpaper_complete_set_wallpaper_uri (object,
                                                 g_steal_pointer (&invocation),
                                                 xdp_request_future_get_object_path (request));

  if (!get_permission (wallpaper,
                       request,
                       app_info,
                       arg_options,
                       arg_parent_window,
                       &permission_granted,
                       &error))
    {
      g_warning ("Getting permission failed: %s", error->message);
      permission_granted = FALSE;
    }

  if (!permission_granted)
    {
      xdp_request_future_emit_response (request,
                                        XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                        NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_autoptr(XdpDbusImplWallpaperSetWallpaperURIResult) result = NULL;
    XdgDesktopPortalResponseEnum response;

    result = dex_await_boxed (xdp_dbus_impl_wallpaper_call_set_wallpaper_uri_future (
        wallpaper->impl,
        xdp_request_future_get_object_path (request),
        xdp_app_info_get_id (app_info),
        arg_parent_window,
        arg_uri,
        g_variant_builder_end (&opt_builder)),
      &error);

    if (result)
      {
        response = result->response;
      }
    else
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Backend call failed: %s", error->message);

        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }

    xdp_request_future_emit_response (request, response, NULL);
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_wallpaper_file (XdpDbusWallpaper      *object,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList           *fd_list,
                           const char            *arg_parent_window,
                           GVariant              *arg_fd,
                           GVariant              *arg_options)
{
  XdpWallpaper *wallpaper = XDP_WALLPAPER (object);
  g_autoptr(XdpRequestFuture) request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) options = NULL;
  gboolean permission_granted;
  g_autofd int fd = -1;
  g_autofree char *uri = NULL;
  g_autoptr(GError) error = NULL;

  {
    g_auto(GVariantBuilder) opt_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                             &opt_builder,
                             wallpaper_options,
                             G_N_ELEMENTS (wallpaper_options),
                             NULL,
                             &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));
  }

  {
    int fd_id;

    g_variant_get (arg_fd, "h", &fd_id);

    if (fd_id >= g_unix_fd_list_get_length (fd_list))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR,
                                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "Bad file descriptor index");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    fd = g_unix_fd_list_get (fd_list, fd_id, &error);
    if (fd == -1)
      {
        g_dbus_method_invocation_return_gerror (invocation, error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  request = dex_await_object (xdp_request_future_new (wallpaper->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (wallpaper->impl),
                                                      arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_wallpaper_complete_set_wallpaper_file (object,
                                                  g_steal_pointer (&invocation),
                                                  NULL,
                                                  xdp_request_future_get_object_path (request));

  if (!get_permission (wallpaper,
                       request,
                       app_info,
                       options,
                       arg_parent_window,
                       &permission_granted,
                       &error))
    {
      g_warning ("Getting permission failed: %s", error->message);
      permission_granted = FALSE;
    }

  if (!permission_granted)
    {
      xdp_request_future_emit_response (request,
                                        XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                        NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_autofree char *path = NULL;

    path = xdp_app_info_get_path_for_fd (app_info, fd, 0, NULL, NULL, &error);

    if (path == NULL)
      {
        g_warning ("Could not get path for fd: %s", error->message);

        xdp_request_future_emit_response (request,
                                          XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                          NULL);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    uri = g_filename_to_uri (path, NULL, NULL);
  }

  {
    g_autoptr(XdpDbusImplWallpaperSetWallpaperURIResult) result = NULL;
    XdgDesktopPortalResponseEnum response;

    result = dex_await_boxed (xdp_dbus_impl_wallpaper_call_set_wallpaper_uri_future (
        wallpaper->impl,
        xdp_request_future_get_object_path (request),
        xdp_app_info_get_id (app_info),
        arg_parent_window,
        uri,
        options),
      &error);

    if (result)
      {
        response = result->response;
      }
    else
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Backend call failed: %s", error->message);

        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }

    xdp_request_future_emit_response (request, response, NULL);
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static void
xdp_wallpaper_iface_init (XdpDbusWallpaperIface *iface)
{
  iface->handle_set_wallpaper_uri = handle_set_wallpaper_uri;
  iface->handle_set_wallpaper_file = handle_set_wallpaper_file;
}

static void
xdp_wallpaper_dispose (GObject *object)
{
  XdpWallpaper *wallpaper = XDP_WALLPAPER (object);

  g_clear_object (&wallpaper->impl);
  g_clear_object (&wallpaper->access_impl);

  G_OBJECT_CLASS (xdp_wallpaper_parent_class)->dispose (object);
}

static void
xdp_wallpaper_init (XdpWallpaper *wallpaper)
{
}

static void
xdp_wallpaper_class_init (XdpWallpaperClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_wallpaper_dispose;
}

static XdpWallpaper *
xdp_wallpaper_new (XdpContext           *context,
                   XdpDbusImplWallpaper *impl,
                   XdpDbusImplAccess    *access_impl)
{
  XdpWallpaper *wallpaper;

  wallpaper = g_object_new (XDP_TYPE_WALLPAPER, NULL);
  wallpaper->context = context; // FIXME there might be problems with the context lifetime
  wallpaper->impl = g_object_ref (impl);
  wallpaper->access_impl = g_object_ref (access_impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (wallpaper->impl), G_MAXINT);

  xdp_dbus_wallpaper_set_version (XDP_DBUS_WALLPAPER (wallpaper), 1);

  return wallpaper;
}

DexFuture *
init_wallpaper (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpWallpaper) wallpaper = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  XdpDbusImplWallpaper *impl;
  XdpDbusImplAccess *access_impl;
  g_autoptr(GVariant) version = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, WALLPAPER_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  access_impl = xdp_context_get_access_impl (context);
  if (access_impl == NULL)
    {
      g_warning ("The wallpaper portal requires an access impl");
      return dex_future_new_false ();
    }

  impl = dex_await_object (xdp_dbus_impl_wallpaper_proxy_new_future (connection,
                                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                                     impl_config->dbus_name,
                                                                     DESKTOP_DBUS_PATH),
                           &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create wallpaper proxy: %s", error->message);
      return dex_future_new_false ();
    }

  wallpaper = xdp_wallpaper_new (context, impl, access_impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&wallpaper)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
