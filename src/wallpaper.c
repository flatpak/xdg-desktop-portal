/*
 * Copyright © 2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "wallpaper.h"
#include "permissions.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "wallpaper"
#define PERMISSION_ID "wallpaper"

typedef struct _Wallpaper Wallpaper;
typedef struct _WallpaperClass WallpaperClass;

struct _Wallpaper
{
  XdpDbusWallpaperSkeleton parent_instance;
};

struct _WallpaperClass
{
  XdpDbusWallpaperSkeletonClass parent_class;
};

static XdpDbusImplWallpaper *impl;
static XdpDbusImplAccess *access_impl;
static Wallpaper *wallpaper;

GType wallpaper_get_type (void) G_GNUC_CONST;
static void wallpaper_iface_init (XdpDbusWallpaperIface *iface);

G_DEFINE_TYPE_WITH_CODE (Wallpaper, wallpaper, XDP_DBUS_TYPE_WALLPAPER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WALLPAPER,
                                                wallpaper_iface_init));

static void
send_response (Request *request,
               guint response)
{
  if (request->exported)
    {
      GVariantBuilder opt_builder;

      g_debug ("sending response: %d", response);
      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }
}

static void
handle_set_wallpaper_uri_done (GObject *source,
                               GAsyncResult *result,
                               gpointer data)
{
  guint response = 2;
  g_autoptr(GError) error = NULL;
  Request *request = data;

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
validate_set_on (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
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
  Request *request = (Request *)task_data;
  const char *parent_window;
  const char *id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;
  GVariantBuilder opt_builder;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariant *options;
  gboolean show_preview = FALSE;
  int fd;
  Permission permission;

  REQUEST_AUTOLOCK (request);

  parent_window = ((const char *)g_object_get_data (G_OBJECT (request), "parent-window"));
  uri = g_strdup ((const char *)g_object_get_data (G_OBJECT (request), "uri"));
  fd = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "fd"));
  options = ((GVariant *)g_object_get_data (G_OBJECT (request), "options"));

  if (uri != NULL && fd != -1)
    {
      g_warning ("Rejecting invalid set-wallpaper request (both URI and fd are set)");
      if (request->exported)
        {
          g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
          xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                          XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                          g_variant_builder_end (&opt_builder));
          request_unexport (request);
        }
      return;
    }


  permission = get_permission_sync (id, PERMISSION_TABLE, PERMISSION_ID);

  if (permission == PERMISSION_NO)
    {
      send_response (request, 2);
      return;
    }

  g_variant_lookup (options, "show-preview", "b", &show_preview);
  if (!show_preview && permission != PERMISSION_YES)
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      GVariantBuilder access_opt_builder;
      g_autofree gchar *app_id = NULL;
      g_autofree gchar *title = NULL;
      g_autofree gchar *subtitle = NULL;
      const gchar *body;

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Allow")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("preferences-desktop-wallpaper-symbolic"));

      if (g_strcmp0 (id, "") != 0)
        {
          g_autoptr(GAppInfo) info = NULL;
          const gchar *name = NULL;

          info = xdp_app_info_load_app_info (request->app_info);

          if (info)
            {
              name = g_app_info_get_display_name (G_APP_INFO (info));
              app_id = xdp_get_app_id_from_desktop_id (g_app_info_get_id (info));
            }
          else
            {
              name = id;
              app_id = g_strdup (id);
            }

          title = g_strdup_printf (_("Allow %s to Set Backgrounds?"), name);
          subtitle = g_strdup_printf (_("%s is requesting to be able to change the background image."), name);
        }
      else
        {
          /* Note: this will set the wallpaper permission for all unsandboxed
           * apps for which an app ID can't be determined.
           */
          g_assert (xdp_app_info_is_host (request->app_info));
          app_id = g_strdup ("");
          title = g_strdup (_("Allow Apps to Set Backgrounds?"));
          subtitle = g_strdup (_("An app is requesting to be able to change the background image."));
        }
      body = _("This permission can be changed at any time from the privacy settings.");

      if (!xdp_dbus_impl_access_call_access_dialog_sync (access_impl,
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

      if (permission == PERMISSION_UNSET)
        set_permission_sync (id, PERMISSION_TABLE, PERMISSION_ID, access_response == 0 ? PERMISSION_YES : PERMISSION_NO);

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
              g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
              xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                              XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                              g_variant_builder_end (&opt_builder));
              request_unexport (request);
            }
          return;
        }

      uri = g_filename_to_uri (path, NULL, NULL);
      g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (uri), g_free);
      close (fd);
      g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (-1));
    }

  impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                       g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                       request->id,
                                                       NULL, &error);

  if (!impl_request)
    {
      g_warning ("Failed to to create wallpaper implementation proxy: %s", error->message);
      send_response (request, 2);
      return;
    }

  request_set_impl_request (request, impl_request);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (options, &opt_builder,
                      wallpaper_options, G_N_ELEMENTS (wallpaper_options),
                      NULL);

  g_debug ("Calling SetWallpaperURI with %s", uri);
  xdp_dbus_impl_wallpaper_call_set_wallpaper_uri (impl,
                                                  request->id,
                                                  id,
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
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_debug ("Handle SetWallpaperURI");

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
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
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  int fd_id, fd;
  g_autoptr(GError) error = NULL;

  g_debug ("Handle SetWallpaperFile");

  g_variant_get (arg_fd, "h", &fd_id);
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

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
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
wallpaper_init (Wallpaper *wallpaper)
{
  xdp_dbus_wallpaper_set_version (XDP_DBUS_WALLPAPER (wallpaper), 1);
}

static void
wallpaper_class_init (WallpaperClass *klass)
{
}

GDBusInterfaceSkeleton *
wallpaper_create (GDBusConnection *connection,
                  const char *dbus_name_access,
                  const char *dbus_name_wallpaper)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_wallpaper_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 dbus_name_wallpaper,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create wallpaper proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);
  wallpaper = g_object_new (wallpaper_get_type (), NULL);

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name_access,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);

  return G_DBUS_INTERFACE_SKELETON (wallpaper);
}
