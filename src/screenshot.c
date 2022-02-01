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

#include <gio/gio.h>

#include "screenshot.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Screenshot Screenshot;
typedef struct _ScreenshotClass ScreenshotClass;

struct _Screenshot
{
  XdpScreenshotSkeleton parent_instance;
};

struct _ScreenshotClass
{
  XdpScreenshotSkeletonClass parent_class;
};

static XdpImplScreenshot *impl;
static Screenshot *screenshot;

GType screenshot_get_type (void) G_GNUC_CONST;
static void screenshot_iface_init (XdpScreenshotIface *iface);

G_DEFINE_TYPE_WITH_CODE (Screenshot, screenshot, XDP_TYPE_SCREENSHOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SCREENSHOT, screenshot_iface_init));

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

      ruri = register_document (uri, xdp_app_info_get_id (request->app_info), FALSE, FALSE, FALSE, &error);
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
  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
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

  if (!xdp_impl_screenshot_call_screenshot_finish (XDP_IMPL_SCREENSHOT (source),
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

static gboolean
handle_screenshot (XdpScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
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
                      screenshot_options, G_N_ELEMENTS (screenshot_options),
                      NULL);

  xdp_impl_screenshot_call_screenshot (impl,
                                       request->id,
                                       xdp_app_info_get_id (request->app_info),
                                       arg_parent_window,
                                       g_variant_builder_end (&opt_builder),
                                       NULL,
                                       screenshot_done,
                                       g_object_ref (request));

  xdp_screenshot_complete_screenshot (object, invocation, request->id);

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

  if (!xdp_impl_screenshot_call_pick_color_finish (XDP_IMPL_SCREENSHOT (source),
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
handle_pick_color (XdpScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
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

  xdp_impl_screenshot_call_pick_color (impl,
                                       request->id,
                                       xdp_app_info_get_id (request->app_info),
                                       arg_parent_window,
                                       g_variant_builder_end (&opt_builder),
                                       NULL,
                                       pick_color_done,
                                       g_object_ref (request));

  xdp_screenshot_complete_pick_color (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
screenshot_iface_init (XdpScreenshotIface *iface)
{
  iface->handle_screenshot = handle_screenshot;
  iface->handle_pick_color = handle_pick_color;
}

static void
screenshot_init (Screenshot *screenshot)
{
  xdp_screenshot_set_version (XDP_SCREENSHOT (screenshot), 2);
}

static void
screenshot_class_init (ScreenshotClass *klass)
{
}

GDBusInterfaceSkeleton *
screenshot_create (GDBusConnection *connection,
                   const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_screenshot_proxy_new_sync (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             dbus_name,
                                             DESKTOP_PORTAL_OBJECT_PATH,
                                             NULL,
                                             &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create screenshot proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  screenshot = g_object_new (screenshot_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (screenshot);
}
