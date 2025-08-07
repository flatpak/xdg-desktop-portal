/*
 * Copyright © 2025 JakobDev
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
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "file-access.h"
#include "xdp-request.h"
#include "xdp-documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _FileAccess FileAccess;
typedef struct _FileAccessClass FileAccessClass;

struct _FileAccess
{
  XdpDbusFileAccessSkeleton parent_instance;
};

struct _FileAccessClass
{
  XdpDbusFileAccessSkeletonClass parent_class;
};

static XdpDbusImplAccess *access_impl;
static FileAccess *file_access;

GType file_access_get_type (void) G_GNUC_CONST;
static void file_access_iface_init (XdpDbusFileAccessIface *iface);

G_DEFINE_TYPE_WITH_CODE (FileAccess, file_access, XDP_DBUS_TYPE_FILE_ACCESS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_FILE_ACCESS,
                                                file_access_iface_init));

static void
send_response (XdpRequest *request,
               guint response,
               GVariant *results)
{
  if (request->exported)
    {
      g_debug ("sending response: %d", response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      xdp_request_unexport (request);
    }
  else
    {
      g_variant_ref_sink (results);
      g_variant_unref (results);
    }
}

static void
handle_send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  XdpRequest *request = XDP_REQUEST (task_data);
  const char *parent_window;
  const char *path;
  GVariant *options;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) new_results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *app_id;
  g_autofree gchar *path_uri = NULL;
  g_autofree gchar *document_uri = NULL;
  gboolean read_only;
  XdpDocumentFlags flags = XDP_DOCUMENT_FLAG_NONE;

  REQUEST_AUTOLOCK (request);

  parent_window = ((const char *)g_object_get_data (G_OBJECT (request), "parent-window"));
  path = ((const char *)g_object_get_data (G_OBJECT (request), "path"));
  options = ((GVariant *)g_object_get_data (G_OBJECT (request), "options"));

  path_uri = g_strdup_printf ("file://%s", path);

  if (xdp_app_info_is_host (request->app_info))
    {
      g_variant_builder_add (&opt_builder, "{sv}", "uri",
                         g_variant_new_string (path_uri));

      send_response (request, 0, g_variant_builder_end (&opt_builder));
      return;
    }

  app_id = xdp_app_info_get_id (request->app_info);

  if (!g_variant_lookup (options, "readonly", "b", &read_only))
    read_only = FALSE;

  // TODO: Check permission
  if (true)
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      g_auto(GVariantBuilder) access_opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autofree gchar *title = NULL;
      g_autofree gchar *subtitle = NULL;
      const gchar *body;
      const gchar *name = NULL;
      g_autoptr(GDesktopAppInfo) info = NULL;
      g_autofree gchar *desktop_id = NULL;

      desktop_id = g_strconcat (app_id, ".desktop", NULL);
      info = g_desktop_app_info_new (desktop_id);

      if (info)
        name = g_app_info_get_display_name (G_APP_INFO (info));
      else
        name = app_id;

      if (read_only)
        {
          title = g_strdup_printf (_("Allow %s to access path"), name);
          subtitle = g_strdup ("");
          body = g_strdup_printf (_("%s wants full access to %s"), name, path);
        }
      else
        {
          title = g_strdup_printf (_("Allow %s to read path"), name);
          subtitle = g_strdup ("");
          body = g_strdup_printf (_("%s wants read access to %s"), name, path);
        }

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
          send_response (request, 2, g_variant_builder_end  (&opt_builder));
          return;
        }

      if (access_response != 0)
        {
          send_response (request, 2, g_variant_builder_end (&opt_builder));
          return;
        }
    }

  if (!read_only)
    flags |= XDP_DOCUMENT_FLAG_WRITABLE;

  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    flags |= XDP_DOCUMENT_FLAG_DIRECTORY;

  document_uri = xdp_register_document (path_uri, app_id, flags, &error);
  if (document_uri == NULL)
    {
      g_warning ("Error registering %s for %s: %s", path_uri, app_id, error->message);
      send_response (request, 2, g_variant_builder_end (&opt_builder));
      return;
    }

  g_variant_builder_add (&opt_builder, "{sv}", "uri",
                         g_variant_new_string (document_uri));

  send_response (request, 0, g_variant_builder_end (&opt_builder));
}

static gboolean
handle_request_path_access (XdpDbusFileAccess *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_parent_window,
                          const char *arg_path,
                          GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_debug ("Handle RequestPathAccess");

  if (!g_file_test (arg_path, G_FILE_TEST_EXISTS))
    {
      g_dbus_method_invocation_return_error (invocation,
                                            XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                            "Path not exists: %s", arg_path);

      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (request), "path", g_strdup (arg_path), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_file_access_complete_request_path_access (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_send_response_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
file_access_iface_init (XdpDbusFileAccessIface *iface)
{
  iface->handle_request_path_access = handle_request_path_access;
}

static void
file_access_init (FileAccess *file_access)
{
  xdp_dbus_file_access_set_version (XDP_DBUS_FILE_ACCESS (file_access), 1);
}

static void
file_access_class_init (FileAccessClass *klass)
{
}

GDBusInterfaceSkeleton *
file_access_create (GDBusConnection *connection,
                    const char *dbus_name_access)
{
  g_autoptr(GError) error = NULL;

  file_access = g_object_new (file_access_get_type (), NULL);

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name_access,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);

  return G_DBUS_INTERFACE_SKELETON (file_access);
}
