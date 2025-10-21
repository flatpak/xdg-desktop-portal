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
#include "xdp-request.h"
#include "xdp-utils.h"

#include "wallpaper.h"

typedef struct _Wallpaper Wallpaper;
typedef struct _WallpaperClass WallpaperClass;

struct _Wallpaper
{
  XdpDbusWallpaperSkeleton parent_instance;

  XdpDbusImplWallpaper *impl;
  XdpDbusImplAccess *access_impl;
};

struct _WallpaperClass
{
  XdpDbusWallpaperSkeletonClass parent_class;
};

GType wallpaper_get_type (void) G_GNUC_CONST;
static void wallpaper_iface_init (XdpDbusWallpaperIface *iface);

G_DEFINE_TYPE_WITH_CODE (Wallpaper, wallpaper, XDP_DBUS_TYPE_WALLPAPER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WALLPAPER,
                                                wallpaper_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Wallpaper, g_object_unref)

static void
send_response (XdpRequest *request,
               guint response)
{
  if (request->exported)
    {
      g_auto(GVariantBuilder) opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      g_debug ("sending response: %d", response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&opt_builder));
      xdp_request_unexport (request);
    }
}

static void
handle_set_wallpaper_uri_done (GObject *source,
                               GAsyncResult *result,
                               gpointer data)
{
  guint response = 2;
  g_autoptr(GError) error = NULL;
  XdpRequest *request = data;

  if (!xdp_dbus_impl_wallpaper_call_set_wallpaper_uri_finish (XDP_DBUS_IMPL_WALLPAPER (source),
                                                              &response,
                                                              result,
                                                              &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  send_response (request, response);
  g_object_unref (request);
}

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

static void
handle_set_wallpaper_in_thread_func (GTask *task,
                                     gpointer source_object,
                                     gpointer task_data,
                                     GCancellable *cancellable)
{
  Wallpaper *wallpaper = (Wallpaper *) source_object;
  XdpRequest *request = XDP_REQUEST (task_data);
  const char *parent_window;
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariant *options;
  gboolean show_preview = FALSE;
  g_autofd int fd = -1;
  XdpPermission permission;

  REQUEST_AUTOLOCK (request);

  parent_window = ((const char *)g_object_get_data (G_OBJECT (request), "parent-window"));
  uri = g_strdup ((const char *)g_object_get_data (G_OBJECT (request), "uri"));
  fd = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "fd"));
  options = ((GVariant *)g_object_get_data (G_OBJECT (request), "options"));

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (-1));

  if (uri != NULL && fd != -1)
    {
      g_warning ("Rejecting invalid set-wallpaper request (both URI and fd are set)");
      if (request->exported)
        {
          xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                          XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                          g_variant_builder_end (&opt_builder));
          xdp_request_unexport (request);
        }
      return;
    }

  permission = xdp_get_permission_sync (request->app_info,
                                        WALLPAPER_PERMISSION_TABLE,
                                        WALLPAPER_PERMISSION_ID);

  if (permission == XDP_PERMISSION_NO)
    {
      send_response (request, 2);
      return;
    }

  g_variant_lookup (options, "show-preview", "b", &show_preview);
  if (!show_preview && permission != XDP_PERMISSION_YES)
    {
      const char *app_id = xdp_app_info_get_id (request->app_info);
      const char *app_name = xdp_app_info_get_app_display_name (request->app_info);
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
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

      if (!xdp_dbus_impl_access_call_access_dialog_sync (wallpaper->access_impl,
                                                         request->id,
                                                         app_id,
                                                         parent_window,
                                                         title,
                                                         subtitle,
                                                         body,
                                                         g_variant_builder_end (&access_opt_builder),
                                                         &access_response,
                                                         &access_results,
                                                         NULL,
                                                         &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);
          send_response (request, 2);
          return;
        }

      if (permission == XDP_PERMISSION_UNSET)
        xdp_set_permission_sync (request->app_info,
                                 WALLPAPER_PERMISSION_TABLE,
                                 WALLPAPER_PERMISSION_ID,
                                 access_response == 0 ?
                                 XDP_PERMISSION_YES : XDP_PERMISSION_NO);

      if (access_response != 0)
        {
          send_response (request, 2);
          return;
        }
    }

  if (!uri)
    {
      g_autofree char *path = NULL;

      path = xdp_app_info_get_path_for_fd (request->app_info, fd, 0, NULL, NULL, &error);
      if (path == NULL)
        {
          g_debug ("Cannot get path for fd: %s", error->message);

          /* Reject the request */
          if (request->exported)
            {
              xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                              XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                              g_variant_builder_end (&opt_builder));
              xdp_request_unexport (request);
            }
          return;
        }

      uri = g_filename_to_uri (path, NULL, NULL);
      g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (uri), g_free);
    }

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (wallpaper->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (wallpaper->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_warning ("Failed to to create wallpaper implementation proxy: %s", error->message);
      send_response (request, 2);
      return;
    }

  xdp_request_set_impl_request (request, impl_request);

  xdp_filter_options (options, &opt_builder,
                      wallpaper_options, G_N_ELEMENTS (wallpaper_options),
                      NULL, NULL);

  g_debug ("Calling SetWallpaperURI with %s", uri);
  xdp_dbus_impl_wallpaper_call_set_wallpaper_uri (wallpaper->impl,
                                                  request->id,
                                                  xdp_app_info_get_id (request->app_info),
                                                  parent_window,
                                                  uri,
                                                  g_variant_builder_end (&opt_builder),
                                                  NULL,
                                                  handle_set_wallpaper_uri_done,
                                                  g_object_ref (request));
}

static gboolean
handle_set_wallpaper_uri (XdpDbusWallpaper *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_parent_window,
                          const char *arg_uri,
                          GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_debug ("Handle SetWallpaperURI");

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_wallpaper_complete_set_wallpaper_uri (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_set_wallpaper_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_wallpaper_file (XdpDbusWallpaper *object,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList *fd_list,
                           const char *arg_parent_window,
                           GVariant *arg_fd,
                           GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  int fd_id, fd;
  g_autoptr(GError) error = NULL;

  g_debug ("Handle SetWallpaperFile");

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

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (fd));
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_wallpaper_complete_set_wallpaper_file (object, invocation, NULL, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_set_wallpaper_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static void
wallpaper_iface_init (XdpDbusWallpaperIface *iface)
{
  iface->handle_set_wallpaper_uri = handle_set_wallpaper_uri;
  iface->handle_set_wallpaper_file = handle_set_wallpaper_file;
}

static void
wallpaper_dispose (GObject *object)
{
  Wallpaper *wallpaper = (Wallpaper *) object;

  g_clear_object (&wallpaper->impl);
  g_clear_object (&wallpaper->access_impl);

  G_OBJECT_CLASS (wallpaper_parent_class)->dispose (object);
}

static void
wallpaper_init (Wallpaper *wallpaper)
{
}

static void
wallpaper_class_init (WallpaperClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = wallpaper_dispose;
}

static Wallpaper *
wallpaper_new (XdpDbusImplWallpaper *impl,
               XdpDbusImplAccess    *access_impl)
{
  Wallpaper *wallpaper;

  wallpaper = g_object_new (wallpaper_get_type (), NULL);
  wallpaper->impl = g_object_ref (impl);
  wallpaper->access_impl = g_object_ref (access_impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (wallpaper->impl), G_MAXINT);

  xdp_dbus_wallpaper_set_version (XDP_DBUS_WALLPAPER (wallpaper), 1);

  return wallpaper;
}

void
init_wallpaper (XdpContext *context)
{
  g_autoptr(Wallpaper) wallpaper = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  XdpDbusImplWallpaper *impl;
  XdpDbusImplAccess *access_impl;
  g_autoptr(GVariant) version = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, WALLPAPER_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  access_impl = xdp_context_get_access_impl (context);
  if (access_impl == NULL)
    {
      g_warning ("The wallpaper portal requires an access impl");
      return;
    }

  impl = xdp_dbus_impl_wallpaper_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 impl_config->dbus_name,
                                                 DESKTOP_DBUS_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create wallpaper proxy: %s", error->message);
      return;
    }

  wallpaper = wallpaper_new (impl, access_impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&wallpaper)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
