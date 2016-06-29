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

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>

#include "open-uri.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

#define TABLE_NAME "desktop-used-apps"
#define USE_DEFAULT_APP_THRESHOLD 5

typedef struct _OpenURI OpenURI;

typedef struct _OpenURIClass OpenURIClass;

struct _OpenURI
{
  XdpOpenURISkeleton parent_instance;
};

struct _OpenURIClass
{
  XdpOpenURISkeletonClass parent_class;
};

static XdpImplAppChooser *app_chooser_impl;
static XdpImplPermissionStore *permission_store_impl;
static OpenURI *open_uri;

GType open_uri_get_type (void) G_GNUC_CONST;
static void open_uri_iface_init (XdpOpenURIIface *iface);

G_DEFINE_TYPE_WITH_CODE (OpenURI, open_uri, XDP_TYPE_OPEN_URI_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_OPEN_URI, open_uri_iface_init));

static gboolean
get_latest_choice_info (const char *app_id,
                        const char *content_type,
                        gchar **latest_chosen_id,
                        gint *latest_chosen_count)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (permission_store_impl,
                                                   TABLE_NAME,
                                                   content_type,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error updating permission store: %s\n", error->message);
      g_clear_error (&error);
    }

  if (out_perms != NULL)
    {
      GVariantIter iter;
      GVariant *child;
      gboolean app_found = FALSE;

      g_variant_iter_init (&iter, out_perms);
      while (!app_found && (child = g_variant_iter_next_value (&iter)))
        {
          const char *child_app_id;
          g_autofree const char **permissions;

          g_variant_get (child, "{&s^a&s}", &child_app_id, &permissions);
          if (g_strcmp0 (child_app_id, app_id) == 0 &&
              permissions != NULL &&
              permissions[0] != NULL)
            {
              g_auto(GStrv) permission_detail = g_strsplit (permissions[0], ":", 2);
              if (g_strv_length (permission_detail) >= 2)
                {
                  *latest_chosen_id = g_strdup (permission_detail[0]);
                  *latest_chosen_count = atoi(permission_detail[1]);
                }
              app_found = TRUE;
            }
          g_variant_unref (child);
        }
    }

  return (*latest_chosen_id != NULL);
}

static void
launch_application_with_uri (const char *choice_id,
                             const char *uri,
                             const char *parent_window)
{
  g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (choice_id));
  g_autoptr(GAppLaunchContext) context = g_app_launch_context_new ();
  GList uris;

  g_app_launch_context_setenv (context, "PARENT_WINDOW_ID", parent_window);

  uris.data = (gpointer)uri;
  uris.next = NULL;

  g_debug ("launching %s\n", choice_id);
  g_app_info_launch_uris (info, &uris, context, NULL);
}

static void
update_permissions_store (const char *app_id,
                          const char *content_type,
                          const char *chosen_id)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *latest_chosen_id = NULL;
  gint latest_chosen_count = 0;
  g_auto(GStrv) in_permissions = NULL;

  if (get_latest_choice_info (app_id, content_type, &latest_chosen_id, &latest_chosen_count) &&
      (g_strcmp0 (chosen_id, latest_chosen_id) == 0))
    {
      /* same app chosen once again: update the counter */
      if (latest_chosen_count >= USE_DEFAULT_APP_THRESHOLD)
        latest_chosen_count = USE_DEFAULT_APP_THRESHOLD;
      else
        latest_chosen_count++;
    }
  else
    {
      /* latest_chosen_id is heap-allocated */
      latest_chosen_id = g_strdup (chosen_id);
      latest_chosen_count = 0;
    }

  in_permissions = (GStrv) g_new0 (char *, 2);
  in_permissions[0] = g_strdup_printf ("%s:%u", latest_chosen_id, latest_chosen_count);

  if (!xdp_impl_permission_store_call_set_permission_sync (permission_store_impl,
                                                           TABLE_NAME,
                                                           TRUE,
                                                           content_type,
                                                           app_id,
                                                           (const char * const*) in_permissions,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Error updating permission store: %s\n", error->message);
      g_clear_error (&error);
    }
}

static void
app_chooser_done (GObject *source,
                  GAsyncResult *result,
                  gpointer data)
{
  Request *request = data;
  guint response;
  GVariant *options;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_app_chooser_call_choose_application_finish (XDP_IMPL_APP_CHOOSER (source),
                                                            &response,
                                                            &options,
                                                            result,
                                                            &error))
    {
      response = 2;
    }

  if (response == 0)
    {
      const char *uri;
      const char *parent_window;
      const char *content_type;
      const char *choice;

      g_variant_lookup (options, "&s", "choice", &choice);

      uri = g_object_get_data (G_OBJECT (request), "uri");
      parent_window = g_object_get_data (G_OBJECT (request), "parent-window");
      content_type = g_object_get_data (G_OBJECT (request), "content-type");

      launch_application_with_uri (choice, uri, parent_window);
      update_permissions_store (request->app_id, content_type, choice);
    }

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request), response, options);
      request_unexport (request);
    }
}

static gboolean
handle_open_uri (XdpOpenURI *object,
                 GDBusMethodInvocation *invocation,
                 const gchar *arg_parent_window,
                 const gchar *arg_uri,
                 GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_auto(GStrv) choices = NULL;
  guint n_choices = 0;
  GList *infos, *l;
  g_autofree char *uri_scheme = NULL;
  g_autofree char *scheme_down = NULL;
  g_autofree char *content_type = NULL;
  g_autofree char *latest_chosen_id = NULL;
  gint latest_chosen_count = 0;
  GVariantBuilder opts_builder;
  gboolean use_first_choice = FALSE;
  int i;

  REQUEST_AUTOLOCK (request);

  uri_scheme = g_uri_parse_scheme (arg_uri);
  if (uri_scheme && uri_scheme[0] != '\0')
    scheme_down = g_ascii_strdown (uri_scheme, -1);

  if ((scheme_down != NULL) && (strcmp (scheme_down, "file") != 0))
    {
      content_type = g_strconcat ("x-scheme-handler/", scheme_down, NULL);
    }
  else
    {
      g_autoptr(GFile) file = g_file_new_for_uri (arg_uri);
      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                     0,
                                                     NULL,
                                                     NULL);
      content_type = g_strdup (g_file_info_get_content_type (info));
    }

  infos = g_app_info_get_recommended_for_type (content_type);
  n_choices = g_list_length (infos);
  choices = g_new (char *, n_choices + 1);
  for (l = infos, i = 0; l; l = l->next)
    {
      GAppInfo *info = l->data;
      choices[i++] = g_strdup (g_app_info_get_id (info));
    }
  choices[i] = NULL;
  g_list_free_full (infos, g_object_unref);

  /* We normally want a dialog to show up at least a few times, but for http[s] we can
     make an exception in case there's only one candidate application to handle it */
  if ((n_choices == 1) &&
      ((g_strcmp0 (scheme_down, "http") == 0) || (g_strcmp0 (scheme_down, "https") == 0)))
    {
      use_first_choice = TRUE;
    }

  if (use_first_choice ||
      (get_latest_choice_info (app_id, content_type, &latest_chosen_id, &latest_chosen_count) &&
       (latest_chosen_count >= USE_DEFAULT_APP_THRESHOLD)))
    {
      /* If a recommended choice is found, just use it and skip the chooser dialog */
      launch_application_with_uri (use_first_choice ? choices[0] : latest_chosen_id,
                                   arg_uri,
                                   arg_parent_window);

      /* We need to close the request before completing, so do it here */
      xdp_request_complete_close (XDP_REQUEST (request), invocation);
      xdp_open_uri_complete_open_uri (object, invocation, request->id);
      return TRUE;
    }

  g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);

  if (latest_chosen_id != NULL)
    {
      /* Add extra options to the request for the backend */
      g_variant_builder_add (&opts_builder,
                             "{sv}",
                             "latest-choice",
                             g_variant_new_string (latest_chosen_id));
    }

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "content-type", g_strdup (content_type), g_free);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (app_chooser_impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (app_chooser_impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_impl_app_chooser_call_choose_application (app_chooser_impl,
                                                request->id,
                                                app_id,
                                                arg_parent_window,
                                                (const char * const *)choices,
                                                g_variant_builder_end (&opts_builder),
                                                NULL,
                                                app_chooser_done,
                                                request);

  xdp_open_uri_complete_open_uri (object, invocation, request->id);

  return TRUE;
}

static void
open_uri_iface_init (XdpOpenURIIface *iface)
{
  iface->handle_open_uri = handle_open_uri;
}

static void
open_uri_init (OpenURI *fc)
{
}

static void
open_uri_class_init (OpenURIClass *klass)
{
}

GDBusInterfaceSkeleton *
open_uri_create (GDBusConnection *connection,
                 const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  app_chooser_impl = xdp_impl_app_chooser_proxy_new_sync (connection,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          dbus_name,
                                                          "/org/freedesktop/portal/desktop",
                                                          NULL, &error);
  if (app_chooser_impl == NULL)
    {
      g_warning ("Failed to create app chooser proxy: %s\n", error->message);
      return NULL;
    }

  permission_store_impl = xdp_impl_permission_store_proxy_new_sync (connection,
                                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                                    NULL, &error);
  if (permission_store_impl == NULL)
    {
      g_warning ("No permission store: %s", error->message);
      return NULL;
    }

  set_proxy_use_threads (G_DBUS_PROXY (app_chooser_impl));

  open_uri = g_object_new (open_uri_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (open_uri);
}

