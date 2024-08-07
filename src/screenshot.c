/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "screenshot.h"
#include "permissions.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "screenshot"
#define PERMISSION_ID "screenshot"

typedef struct _Screenshot Screenshot;
typedef struct _ScreenshotClass ScreenshotClass;

struct _Screenshot
{
  XdpDbusScreenshotSkeleton parent_instance;
};

struct _ScreenshotClass
{
  XdpDbusScreenshotSkeletonClass parent_class;
};

static XdpDbusImplScreenshot *impl;
static XdpDbusImplAccess *access_impl;
static guint32 impl_version;
static Screenshot *screenshot;

GType screenshot_get_type (void) G_GNUC_CONST;
static void screenshot_iface_init (XdpDbusScreenshotIface *iface);

G_DEFINE_TYPE_WITH_CODE (Screenshot, screenshot, XDP_DBUS_TYPE_SCREENSHOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SCREENSHOT,
                                                screenshot_iface_init));

static void
send_response (Request *request,
               guint response,
               GVariant *results)
{
  if (request->exported)
    {
      g_debug ("sending response: %d", response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      request_unexport (request);
    }
}

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  Request *request = task_data;
  GVariantBuilder results;
  guint response;
  GVariant *options;
  g_autoptr(GError) error = NULL;
  const char *retval;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  if (response != 0)
    goto out;

  retval = g_object_get_data (G_OBJECT (task), "retval");
  if (g_strcmp0 (retval, "url") == 0)
    {
      const char *uri;
      g_autofree char *ruri = NULL;

      if (!g_variant_lookup (options, "uri", "&s", &uri))
        {
          g_warning ("No URI was provided");
          goto out;
        }

      if (xdp_app_info_is_host (request->app_info))
        ruri = g_strdup (uri);
      else
        ruri = register_document (uri, xdp_app_info_get_id (request->app_info), DOCUMENT_FLAG_DELETABLE, &error);

      if (ruri == NULL)
        g_warning ("Failed to register %s: %s", uri, error->message);
      else
        g_variant_builder_add (&results, "{&sv}", "uri", g_variant_new_string (ruri));
    }
  else if (g_strcmp0 (retval, "color") == 0)
    {
      double red, green, blue;

      if (!g_variant_lookup (options, "color", "(ddd)", &red, &green, &blue))
        {
          g_warning ("No color was provided");
          goto out;
        }

      g_variant_builder_add (&results, "{&sv}", "color", g_variant_new ("(ddd)", red, green, blue));
    }
  else
    {
      g_warning ("Don't know what to return");
    }

out:
  send_response (request, response, g_variant_builder_end (&results));
}

static void
screenshot_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_screenshot_call_screenshot_finish (XDP_DBUS_IMPL_SCREENSHOT (source),
                                                        &response,
                                                        &options,
                                                        result,
                                                        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_object_set_data (G_OBJECT (task), "retval", "url");
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static XdpOptionKey screenshot_options[] = {
  { "modal", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "interactive", G_VARIANT_TYPE_BOOLEAN, NULL }
};

static void
handle_screenshot_in_thread_func (GTask *task,
                                  gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;
  Permission permission;
  GVariant *options;
  gboolean permission_store_checked = FALSE;
  gboolean interactive;
  const char *parent_window;
  const char *app_id;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  app_id = xdp_app_info_get_id (request->app_info);
  parent_window = ((const char *)g_object_get_data (G_OBJECT (request), "parent-window"));
  options = ((GVariant *)g_object_get_data (G_OBJECT (request), "options"));

  if (xdp_dbus_impl_screenshot_get_version (impl) < 2)
    goto query_impl;

  permission = get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);

  if (!g_variant_lookup (options, "interactive", "b", &interactive))
    interactive = FALSE;

  if (!interactive && permission != PERMISSION_YES)
    {
      g_autoptr(GVariant) access_results = NULL;
      GVariantBuilder access_opt_builder;
      g_autofree gchar *subtitle = NULL;
      g_autofree gchar *title = NULL;
      const gchar *body;
      guint access_response = 2;

      if (permission == PERMISSION_NO)
        {
          send_response (request, 2, g_variant_builder_end (&opt_builder));
          return;
        }

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Allow")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("applets-screenshooter-symbolic"));

      if (g_strcmp0 (app_id, "") != 0)
        {
          g_autoptr(GDesktopAppInfo) info = NULL;
          g_autofree gchar *id = NULL;
          const gchar *name = NULL;

          id = g_strconcat (app_id, ".desktop", NULL);
          info = g_desktop_app_info_new (id);

          if (info)
            name = g_app_info_get_display_name (G_APP_INFO (info));
          else
            name = app_id;

          title = g_strdup_printf (_("Allow %s to Take Screenshots?"), name);
          subtitle = g_strdup_printf (_("%s wants to be able to take screenshots at any time."), name);
        }
      else
        {
          /* Note: this will set the wallpaper permission for all unsandboxed
           * apps for which an app ID can't be determined.
           */
          g_assert (xdp_app_info_is_host (request->app_info));
          title = g_strdup (_("Allow Apps to Take Screenshots?"));
          subtitle = g_strdup (_("An app wants to be able to take screenshots at any time."));
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
          send_response (request, 2, g_variant_builder_end (&opt_builder));
          return;
        }

      if (permission == PERMISSION_UNSET)
        set_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID, access_response == 0 ? PERMISSION_YES : PERMISSION_NO);

      if (access_response != 0)
        {
          send_response (request, 2, g_variant_builder_end (&opt_builder));
          return;
        }
    }

  permission_store_checked = TRUE;

query_impl:

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_warning ("Failed to to create screenshot implementation proxy: %s", error->message);
      send_response (request, 2, g_variant_builder_end (&opt_builder));
      return;
    }

  request_set_impl_request (request, impl_request);

  xdp_filter_options (options, &opt_builder,
                      screenshot_options, G_N_ELEMENTS (screenshot_options),
                      NULL);
  if (permission_store_checked)
    {
      g_variant_builder_add (&opt_builder, "{sv}", "permission_store_checked",
                             g_variant_new_boolean (TRUE));
    }

  g_debug ("Calling Screenshot with interactive=%d", interactive);
  xdp_dbus_impl_screenshot_call_screenshot (impl,
                                            request->id,
                                            app_id,
                                            parent_window,
                                            g_variant_builder_end (&opt_builder),
                                            NULL,
                                            screenshot_done,
                                            g_object_ref (request));

}

static gboolean
handle_screenshot (XdpDbusScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_debug ("Handle Screenshot");

  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_screenshot_complete_screenshot (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_screenshot_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
pick_color_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_screenshot_call_pick_color_finish (XDP_DBUS_IMPL_SCREENSHOT (source),
                                                        &response,
                                                        &options,
                                                        result,
                                                        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_object_set_data (G_OBJECT (task), "retval", "color");
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static XdpOptionKey pick_color_options[] = {
};

static gboolean
handle_pick_color (XdpDbusScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;

  REQUEST_AUTOLOCK (request);

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &opt_builder,
                      pick_color_options, G_N_ELEMENTS (pick_color_options),
                      NULL);

  xdp_dbus_impl_screenshot_call_pick_color (impl,
                                            request->id,
                                            xdp_app_info_get_id (request->app_info),
                                            arg_parent_window,
                                            g_variant_builder_end (&opt_builder),
                                            NULL,
                                            pick_color_done,
                                            g_object_ref (request));

  xdp_dbus_screenshot_complete_pick_color (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
screenshot_iface_init (XdpDbusScreenshotIface *iface)
{
  iface->handle_screenshot = handle_screenshot;
  iface->handle_pick_color = handle_pick_color;
}

static void
screenshot_init (Screenshot *screenshot)
{
  /* Before there was a version property, the version was hardcoded to 2, so
   * make sure we retain that behaviour */
  impl_version = 2;
  xdp_dbus_screenshot_set_version (XDP_DBUS_SCREENSHOT (screenshot),
                                   impl_version);
}

static void
screenshot_class_init (ScreenshotClass *klass)
{
}

GDBusInterfaceSkeleton *
screenshot_create (GDBusConnection *connection,
                   const char *dbus_name_access,
                   const char *dbus_name_screenshot)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) version = NULL;

  impl = xdp_dbus_impl_screenshot_proxy_new_sync (connection,
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  dbus_name_screenshot,
                                                  DESKTOP_PORTAL_OBJECT_PATH,
                                                  NULL,
                                                  &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create screenshot proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  /* Set the version if supported; otherwise fallback to hardcoded version 2 */
  version = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (impl), "version");
  impl_version = (version != NULL) ? g_variant_get_uint32 (version) : 2;

  screenshot = g_object_new (screenshot_get_type (), NULL);

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name_access,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);

  return G_DBUS_INTERFACE_SKELETON (screenshot);
}
