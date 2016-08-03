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
#include "xdp-utils.h"
#include "permissions.h"
#include "documents.h"

#define TABLE_NAME "desktop-used-apps"

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

enum {
  PERM_APP_ID,
  PERM_APP_COUNT,
  PERM_APP_THRESHOLD,
  LAST_PERM
};

static XdpImplAppChooser *impl;
static OpenURI *open_uri;

GType open_uri_get_type (void) G_GNUC_CONST;
static void open_uri_iface_init (XdpOpenURIIface *iface);

G_DEFINE_TYPE_WITH_CODE (OpenURI, open_uri, XDP_TYPE_OPEN_URI_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_OPEN_URI, open_uri_iface_init));

static void
parse_permissions (const char **permissions,
                   char **app_id,
                   gint *app_count,
                   gint *app_threshold,
                   gboolean *always_ask)
{
  char *perms_id = NULL;
  gint perms_count = 0;
  gint perms_threshold = G_MAXINT;
  gboolean perms_always_ask = TRUE;

  if ((permissions != NULL) &&
      (permissions[PERM_APP_ID] != NULL) &&
      (permissions[PERM_APP_COUNT] != NULL))
    {
      perms_id = g_strdup (permissions[PERM_APP_ID]);
      perms_count = atoi (permissions[PERM_APP_COUNT]);
      if (permissions[PERM_APP_THRESHOLD] != NULL)
        {
          g_autofree char *threshold = g_strdup (permissions[PERM_APP_THRESHOLD]);
          if (g_strstrip(threshold)[0] != '\0')
            {
              perms_threshold = atoi (permissions[PERM_APP_THRESHOLD]);
              perms_always_ask = FALSE;
            }
        }
    }

  *app_id = perms_id;
  *app_count = perms_count;
  *app_threshold = perms_threshold;
  *always_ask = perms_always_ask;
}

static gboolean
get_latest_choice_info (const char *app_id,
                        const char *content_type,
                        gchar **latest_id,
                        gint *latest_count,
                        gint *latest_threshold,
                        gboolean *always_ask)
{
  char *choice_id = NULL;
  gint choice_count = 0;
  gint choice_threshold = G_MAXINT;
  gboolean choice_always_ask = TRUE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   content_type,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
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
          if (g_strcmp0 (child_app_id, app_id) == 0)
            {
              parse_permissions (permissions, &choice_id, &choice_count, &choice_threshold, &choice_always_ask);
              app_found = TRUE;
            }
          g_variant_unref (child);
        }
    }

  *latest_id = choice_id;
  *latest_count = choice_count;
  *latest_threshold = choice_threshold;
  *always_ask = choice_always_ask;

  return (choice_id != NULL);
}

static gboolean
is_sandboxed (GDesktopAppInfo *info)
{
  g_autofree char *exec;

  exec = g_desktop_app_info_get_string (info, G_KEY_FILE_DESKTOP_KEY_EXEC);
  return strstr (exec, "flatpak run ") != NULL;
}

static void
launch_application_with_uri (const char *choice_id,
                             const char *uri,
                             const char *parent_window,
                             gboolean writable)
{
  g_autofree char *desktop_id = g_strconcat (choice_id, ".desktop", NULL);
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);
  g_autoptr(GAppLaunchContext) context = g_app_launch_context_new ();
  g_autofree char *ruri = NULL;
  GList uris;

  if (is_sandboxed (info))
    {
      g_debug ("registering %s for %s", uri, choice_id);
      ruri = register_document (uri, choice_id, FALSE, writable, NULL);
    }
  else
    ruri = g_strdup (uri);

  g_app_launch_context_setenv (context, "PARENT_WINDOW_ID", parent_window);

  uris.data = (gpointer)ruri;
  uris.next = NULL;

  g_debug ("launching %s %s", choice_id, ruri);
  g_app_info_launch_uris (G_APP_INFO (info), &uris, context, NULL);
}

static void
update_permissions_store (const char *app_id,
                          const char *content_type,
                          const char *chosen_id)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *latest_id;
  gint latest_count;
  gint latest_threshold;
  gboolean always_ask;
  g_auto(GStrv) in_permissions = NULL;

  if (get_latest_choice_info (app_id, content_type, &latest_id, &latest_count, &latest_threshold, &always_ask) &&
      (g_strcmp0 (chosen_id, latest_id) == 0))
    {
      /* same app chosen once again: update the counter */
      if (latest_count >= latest_threshold)
        latest_count = latest_threshold;
      else
        latest_count++;
    }
  else
    {
      /* latest_id is heap-allocated */
      latest_id = g_strdup (chosen_id);
      latest_count = 0;
    }

  in_permissions = (GStrv) g_new0 (char *, LAST_PERM + 1);
  in_permissions[PERM_APP_ID] = g_strdup (latest_id);
  in_permissions[PERM_APP_COUNT] = g_strdup_printf ("%u", latest_count);
  in_permissions[PERM_APP_THRESHOLD] = always_ask ? g_strdup ("") : g_strdup_printf ("%u", latest_threshold);

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           TABLE_NAME,
                                                           TRUE,
                                                           content_type,
                                                           app_id,
                                                           (const char * const*) in_permissions,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
      g_clear_error (&error);
    }
}

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  guint response;
  GVariant *options;
  const char *choice;
  GVariantBuilder opt_builder;

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (response != 0)
    goto out;

  if (g_variant_lookup (options, "choice", "&s", &choice))
    {
      const char *uri;
      const char *parent_window;
      gboolean writable;
      const char *content_type;

      uri = (const char *)g_object_get_data (G_OBJECT (request), "uri");
      parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");
      writable = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "writable"));
      content_type = (const char *)g_object_get_data (G_OBJECT (request), "content-type");

      launch_application_with_uri (choice, uri, parent_window, writable);
      update_permissions_store (request->app_id, content_type, choice);
    }

out:
  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request), response, g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }
}

static void
app_chooser_done (GObject *source,
                  GAsyncResult *result,
                  gpointer data)
{
  g_autoptr (Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_app_chooser_call_choose_application_finish (XDP_IMPL_APP_CHOOSER (source),
                                                            &response,
                                                            &options,
                                                            result,
                                                            &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static void
resolve_scheme_and_content_type (const char *uri,
                                 char **scheme,
                                 gchar **content_type)
{
  g_autofree char *uri_scheme = NULL;

  uri_scheme = g_uri_parse_scheme (uri);
  if (uri_scheme && uri_scheme[0] != '\0')
    *scheme = g_ascii_strdown (uri_scheme, -1);

  if ((*scheme != NULL) && (strcmp (*scheme, "file") != 0))
    {
      *content_type = g_strconcat ("x-scheme-handler/", *scheme, NULL);
    }
  else
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GFile) file = g_file_new_for_uri (uri);
      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                     0,
                                                     NULL,
                                                     &error);

      if (info != NULL)
        {
          *content_type = g_strdup (g_file_info_get_content_type (info));
          g_debug ("Content type for uri %s: %s", uri, *content_type);
        }
      else
        {
          g_debug ("Failed to fetch content type for uri %s: %s", uri, error->message);
        }
    }
}

static void
find_recommended_choices (const char *scheme,
                          const char *content_type,
                          GStrv *choices,
                          gboolean *use_first_choice)
{
  GList *infos, *l;
  guint n_choices = 0;
  GStrv result = NULL;
  int i;

  infos = g_app_info_get_recommended_for_type (content_type);
  n_choices = g_list_length (infos);
  result = g_new (char *, n_choices + 1);
  for (l = infos, i = 0; l; l = l->next)
    {
      const char *desktop_id;

      GAppInfo *info = l->data;
      desktop_id = g_app_info_get_id (info);
      result[i++] = g_strndup (desktop_id, strlen (desktop_id) - strlen (".desktop"));
    }
  result[i] = NULL;
  g_list_free_full (infos, g_object_unref);

  /* We normally want a dialog to show up at least a few times, but for http[s] we can
     make an exception in case there's only one candidate application to handle it */
  if ((n_choices == 1) &&
      ((g_strcmp0 (scheme, "http") == 0) || (g_strcmp0 (scheme, "https") == 0)))
    {
      *use_first_choice = TRUE;
    }

  *choices = result;
}

static void
handle_open_in_thread_func (GTask *task,
                            gpointer source_object,
                            gpointer task_data,
                            GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *parent_window;
  const char *uri;
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_auto(GStrv) choices = NULL;
  g_autofree char *scheme = NULL;
  g_autofree char *content_type = NULL;
  g_autofree char *latest_id;
  gint latest_count;
  gint latest_threshold;
  gboolean always_ask;
  GVariantBuilder opts_builder;
  gboolean use_first_choice = FALSE;
  gboolean writable = FALSE;

  parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");
  uri = (const char *)g_object_get_data (G_OBJECT (request), "uri");
  writable = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "writable"));

  REQUEST_AUTOLOCK (request);

  resolve_scheme_and_content_type (uri, &scheme, &content_type);
  if (content_type == NULL)
    {
      /* Reject the request */
      if (request->exported)
        {
          g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
          xdp_request_emit_response (XDP_REQUEST (request), 2, g_variant_builder_end (&opts_builder));
          request_unexport (request);
        }
      return;
    }

  find_recommended_choices (scheme, content_type, &choices, &use_first_choice);
  get_latest_choice_info (app_id, content_type, &latest_id, &latest_count, &latest_threshold, &always_ask);

  if (use_first_choice || (!always_ask && (latest_count >= latest_threshold)))
    {
      /* If a recommended choice is found, just use it and skip the chooser dialog */
      launch_application_with_uri (use_first_choice ? choices[0] : latest_id,
                                   uri,
                                   parent_window,
                                   writable);

      if (request->exported)
        {
          g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
          xdp_request_emit_response (XDP_REQUEST (request), 0, g_variant_builder_end (&opts_builder));
          request_unexport (request);
        }
      return;
    }

  g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);

  if (latest_id != NULL)
    {
      /* Add extra options to the request for the backend */
      g_variant_builder_add (&opts_builder,
                             "{sv}",
                             "last_choice",
                             g_variant_new_string (latest_id));
    }

  g_object_set_data_full (G_OBJECT (request), "content-type", g_strdup (content_type), g_free);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, NULL);

  request_set_impl_request (request, impl_request);

  xdp_impl_app_chooser_call_choose_application (impl,
                                                request->id,
                                                app_id,
                                                parent_window,
                                                (const char * const *)choices,
                                                g_variant_builder_end (&opts_builder),
                                                NULL,
                                                app_chooser_done,
                                                g_object_ref (request));
}

static gboolean
handle_open_uri (XdpOpenURI *object,
                 GDBusMethodInvocation *invocation,
                 const gchar *arg_parent_window,
                 const gchar *arg_uri,
                 GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  gboolean writable;

  if (!g_variant_lookup (arg_options, "writable", "b", &writable))
    writable = FALSE;

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data (G_OBJECT (request), "writable", GINT_TO_POINTER (writable));

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_open_uri_complete_open_uri (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_open_in_thread_func);

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
                 const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_app_chooser_proxy_new_sync (connection,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              dbus_name,
                                              DESKTOP_PORTAL_OBJECT_PATH,
                                              NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create app chooser proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  open_uri = g_object_new (open_uri_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (open_uri);
}

