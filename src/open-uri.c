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
#include <unistd.h>
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

#define FILE_MANAGER_DBUS_NAME "org.freedesktop.FileManager1"
#define FILE_MANAGER_DBUS_IFACE "org.freedesktop.FileManager1"
#define FILE_MANAGER_DBUS_PATH "/org/freedesktop/FileManager1"

#define FILE_MANAGER_SHOW_ITEMS "ShowItems"

#define PERMISSION_TABLE "desktop-used-apps"

#define DEFAULT_THRESHOLD 3

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
static GAppInfoMonitor *monitor;
static XdpImplLockdown *lockdown;

GType open_uri_get_type (void) G_GNUC_CONST;
static void open_uri_iface_init (XdpOpenURIIface *iface);

G_DEFINE_TYPE_WITH_CODE (OpenURI, open_uri, XDP_TYPE_OPEN_URI_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_OPEN_URI, open_uri_iface_init));

static void
parse_permissions (const char **permissions,
                   char **app_id,
                   gint *app_count,
                   gint *app_threshold)
{
  char *perms_id = NULL;
  gint perms_count = 0;
  gint perms_threshold = DEFAULT_THRESHOLD;

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
            perms_threshold = atoi (permissions[PERM_APP_THRESHOLD]);
        }
    }

  *app_id = perms_id;
  *app_count = perms_count;
  *app_threshold = perms_threshold;
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
  int choice_count = 0;
  int choice_threshold = DEFAULT_THRESHOLD;
  gboolean ask = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSION_TABLE,
                                                   content_type,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      /* Not finding an entry for the content type in the permission store is perfectly ok */
      if (!g_error_matches (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND))
        g_warning ("Unable to retrieve info for '%s' in the %s table of the permission store: %s",
                   content_type, PERMISSION_TABLE, error->message);
      g_clear_error (&error);
    }

  if (out_data != NULL)
    {
      g_autoptr(GVariant) data = g_variant_get_child_value (out_data, 0);
      if (g_variant_is_of_type (data, G_VARIANT_TYPE_VARDICT))
        g_variant_lookup (data, "always-ask", "b", &ask);
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
              parse_permissions (permissions, &choice_id, &choice_count, &choice_threshold);
              app_found = TRUE;
            }
          g_variant_unref (child);
        }
    }

  *latest_id = choice_id;
  *latest_count = choice_count;
  *latest_threshold = choice_threshold;
  *always_ask = ask;

  g_debug ("Found in permission store: handler: %s, count: %d / %d, always ask: %d",
           choice_id, choice_count, choice_threshold, ask);

  return (choice_id != NULL);
}

static gboolean
is_sandboxed (GDesktopAppInfo *info)
{
  g_autofree char *flatpak = NULL;

  flatpak = g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (info), "X-Flatpak");

  return flatpak != NULL;
}

/* This returns the desktop file basename without extension.
 * We cant' just use the flatpak ID, since flatpaks are allowed
 * to export 'sub ids', like the org.libreoffice.LibreOffice
 * flatpak exporting org.libreoffice.LibreOffice.Impress.desktop,
 * and we need to track the actual handlers.
 *
 * We still strip the .desktop extension, since that is what
 * the backends expect.
 */
static char *
get_app_id (GAppInfo *info)
{
  const char *desktop_id;

  desktop_id = g_app_info_get_id (info);

  return g_strndup (desktop_id, strlen (desktop_id) - strlen (".desktop"));
}

static gboolean
is_file_uri (const char *uri)
{
  g_autofree char *scheme = NULL;

  scheme = g_uri_parse_scheme (uri);

  if (g_strcmp0 (scheme, "file") == 0)
    return TRUE;

  return FALSE;
}

static gboolean
launch_application_with_uri (const char *choice_id,
                             const char *uri,
                             const char *parent_window,
                             gboolean    writable,
                             const char *activation_token,
                             GError    **error)
{
  g_autofree char *desktop_id = g_strconcat (choice_id, ".desktop", NULL);
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);
  g_autoptr(GAppLaunchContext) context = g_app_launch_context_new ();
  g_autofree char *ruri = NULL;
  GList uris;

  if (info == NULL)
    {
      g_debug ("Cannot launch %s because desktop file does not exist", desktop_id);
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND, "Desktop file %s does not exist", desktop_id);
      return FALSE;
    }

  g_debug ("Launching %s %s", choice_id, uri);

  if (is_sandboxed (info) && is_file_uri (uri))
    {
      g_autoptr(GError) local_error = NULL;

      g_debug ("Registering %s for %s", uri, choice_id);

      ruri = register_document (uri, choice_id, FALSE, writable, FALSE, &local_error);
      if (ruri == NULL)
        {
          g_warning ("Error registering %s for %s: %s", uri, choice_id, local_error->message);
          g_propagate_error (error, local_error);
          return FALSE;
        }
    }
  else
    ruri = g_strdup (uri);

  g_app_launch_context_setenv (context, "PARENT_WINDOW_ID", parent_window);

  if (activation_token)
    g_app_launch_context_setenv (context, "XDG_ACTIVATION_TOKEN", activation_token);

  uris.data = (gpointer)ruri;
  uris.next = NULL;

  g_app_info_launch_uris (G_APP_INFO (info), &uris, context, error);

  return TRUE;
}

static void
update_permissions_store (const char *app_id,
                          const char *content_type,
                          const char *chosen_id)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *latest_id = NULL;
  gint latest_count;
  gint latest_threshold;
  g_auto(GStrv) in_permissions = NULL;
  gboolean ask;

  if (get_latest_choice_info (app_id, content_type,
                              &latest_id, &latest_count, &latest_threshold, &ask) &&
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
      latest_id = g_strdup (chosen_id);
      latest_count = 1;
    }

  in_permissions = (GStrv) g_new0 (char *, LAST_PERM + 1);
  in_permissions[PERM_APP_ID] = g_strdup (latest_id);
  in_permissions[PERM_APP_COUNT] = g_strdup_printf ("%u", latest_count);
  in_permissions[PERM_APP_THRESHOLD] = g_strdup_printf ("%u", latest_threshold);

  g_debug ("updating permissions for %s: content-type %s, handler %s, count %s / %s",
           app_id,
           content_type,
           in_permissions[PERM_APP_ID],
           in_permissions[PERM_APP_COUNT],
           in_permissions[PERM_APP_THRESHOLD]);


  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           PERMISSION_TABLE,
                                                           TRUE,
                                                           content_type,
                                                           app_id,
                                                           (const char * const*) in_permissions,
                                                           NULL,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
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
      const char *activation_token = NULL;

      g_debug ("Received choice %s", choice);

      uri = (const char *)g_object_get_data (G_OBJECT (request), "uri");
      parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");
      writable = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "writable"));
      content_type = (const char *)g_object_get_data (G_OBJECT (request), "content-type");

      g_variant_lookup (options, "activation_token", "&s", &activation_token);

      if (launch_application_with_uri (choice, uri, parent_window, writable, activation_token, NULL))
        update_permissions_store (xdp_app_info_get_id (request->app_info), content_type, choice);
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
      g_dbus_error_strip_remote_error (error);
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
                                 char **content_type)
{
  g_autofree char *uri_scheme = NULL;

  uri_scheme = g_uri_parse_scheme (uri);
  if (uri_scheme && uri_scheme[0] != '\0')
    *scheme = g_ascii_strdown (uri_scheme, -1);

  if (*scheme == NULL)
    return;

  if (strcmp (*scheme, "file") == 0)
    {
      g_debug ("Not handling file uri %s", uri);
      return;
    }

  *content_type = g_strconcat ("x-scheme-handler/", *scheme, NULL);
  g_debug ("Content type for %s uri %s: %s", uri, *scheme, *content_type);
}

static void
get_content_type_for_file (const char  *path,
                           char       **content_type)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                 G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                 0,
                                                 NULL,
                                                 &error);

  if (info != NULL)
    {
      *content_type = g_strdup (g_file_info_get_content_type (info));
      g_debug ("Content type for file %s: %s", path, *content_type);
    }
  else
    {
      g_debug ("Failed to fetch content type for file %s: %s", path, error->message);
    }
}

static gboolean
should_use_default_app (const char *scheme,
                        const char *content_type)
{
  const char *skipped_schemes[] = {
    "http",
    "https",
    "ftp",
    "mailto",
    "webcal",
    "calendar",
    NULL
  };

  /* We skip the app chooser for Internet URIs, to be open in the browser,
   * mail client, or calendar, as well as for directories to be opened in
   * the file manager */
  if (g_strv_contains (skipped_schemes, scheme) ||
      g_strcmp0 (content_type, "inode/directory") == 0)
    {
      g_debug ("Can skip app chooser for %s", content_type);
      return TRUE;
    }

  return FALSE;
}

static void
find_recommended_choices (const char *scheme,
                          const char *content_type,
                          char **default_app,
                          GStrv *choices,
                          guint *choices_len)
{
  GAppInfo *info;
  GList *infos, *l;
  guint n_choices = 0;
  GStrv result = NULL;
  int i;

  info = g_app_info_get_default_for_type (content_type, FALSE);

  if (info != NULL)
    {
      *default_app = get_app_id (info);
      g_debug ("Default handler %s for %s, %s", *default_app, scheme, content_type);
    }
  else
    {
      *default_app = NULL;
      g_debug ("No default handler for %s, %s", scheme, content_type);
    }

  infos = g_app_info_get_recommended_for_type (content_type);
  /* Use fallbacks if we have no recommended application for this type */
  if (!infos)
    infos = g_app_info_get_all_for_type (content_type);

  n_choices = g_list_length (infos);
  result = g_new (char *, n_choices + 1);
  for (l = infos, i = 0; l; l = l->next)
    {
      info = l->data;
      result[i++] = get_app_id (info);
    }
  result[i] = NULL;
  g_list_free_full (infos, g_object_unref);

  {
    g_autofree char *a = g_strjoinv (", ", result);
    g_debug ("Recommended handlers for %s, %s: %s", scheme, content_type, a);
  }

  *choices = result;
  *choices_len = n_choices;
}

static void
app_info_changed (GAppInfoMonitor *monitor,
                  Request *request)
{
  const char *scheme;
  const char *content_type;
  g_autofree char *default_app = NULL;
  g_auto(GStrv) choices = NULL;
  guint n_choices;

  scheme = (const char *)g_object_get_data (G_OBJECT (request), "scheme");
  content_type = (const char *)g_object_get_data (G_OBJECT (request), "content-type");
  find_recommended_choices (scheme, content_type, &default_app, &choices, &n_choices);

  xdp_impl_app_chooser_call_update_choices (impl,
                                            request->id,
                                            (const char * const *)choices,
                                            NULL,
                                            NULL,
                                            NULL);
}

static gboolean
app_exists (const char *app_id)
{
  g_autoptr(GDesktopAppInfo) info = NULL;
  g_autofree gchar *with_desktop = NULL;

  g_return_val_if_fail (app_id != NULL, FALSE);

  with_desktop = g_strconcat (app_id, ".desktop", NULL);
  info = g_desktop_app_info_new (with_desktop);
  return (info != NULL);
}

static void
handle_open_in_thread_func (GTask *task,
                            gpointer source_object,
                            gpointer task_data,
                            GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *parent_window;
  const char *app_id = xdp_app_info_get_id (request->app_info);
  const char *activation_token;
  g_autofree char *uri = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autofree char *default_app = NULL;
  g_auto(GStrv) choices = NULL;
  guint n_choices;
  g_autofree char *scheme = NULL;
  g_autofree char *content_type = NULL;
  g_autofree char *latest_id = NULL;
  g_autofree char *basename = NULL;
  gint latest_count;
  gint latest_threshold;
  gboolean ask_for_content_type;
  GVariantBuilder opts_builder;
  gboolean skip_app_chooser = FALSE;
  g_auto(XdpFd) fd = -1;
  gboolean writable = FALSE;
  gboolean ask = FALSE;
  gboolean open_dir = FALSE;
  gboolean use_default_app = FALSE;
  const char *reason;

  parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");
  uri = g_strdup ((const char *)g_object_get_data (G_OBJECT (request), "uri"));
  fd = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "fd"));
  writable = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "writable"));
  ask = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "ask"));
  open_dir = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "open-dir"));
  activation_token = (const char *)g_object_get_data (G_OBJECT (request), "activation-token");

  REQUEST_AUTOLOCK (request);

  /* Verify that either uri or fd is set, not both */
  if (uri != NULL && fd != -1)
    {
      g_warning ("Rejecting invalid open-uri request (both URI and fd are set)");
      if (request->exported)
        {
          g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
          xdp_request_emit_response (XDP_REQUEST (request),
                                     XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                     g_variant_builder_end (&opts_builder));
          request_unexport (request);
        }
      return;
    }

  if (uri)
    {
      resolve_scheme_and_content_type (uri, &scheme, &content_type);
      if (content_type == NULL)
        {
          /* Reject the request */
          if (request->exported)
            {
              g_debug ("Rejecting open request as content-type couldn't be fetched for '%s'", uri);
              g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
              xdp_request_emit_response (XDP_REQUEST (request),
                                         XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                         g_variant_builder_end (&opts_builder));
              request_unexport (request);
            }
          return;
        }
    }
  else
    {
      g_autofree char *path = NULL;
      gboolean fd_is_writable;
      g_autoptr(GError) local_error = NULL;

      path = xdp_app_info_get_path_for_fd (request->app_info, fd, 0, NULL, &fd_is_writable, &local_error);
      if (path == NULL ||
          (writable && !fd_is_writable) ||
          (!xdp_app_info_is_host (request->app_info) && !writable && fd_is_writable))
        {
          /* Reject the request */
          if (path == NULL)
            {
              g_debug ("Rejecting open request: %s", local_error->message);
            }
          else
            {
              g_debug ("Rejecting open request for %s as opening %swritable but fd is %swritable",
                       path, writable ? "" : "not ", fd_is_writable ? "" : "not ");
            }

          if (request->exported)
            {
              g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
              xdp_request_emit_response (XDP_REQUEST (request),
                                         XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                         g_variant_builder_end (&opts_builder));
              request_unexport (request);
            }
          return;
        }

      if (open_dir)
        {
          g_autofree char *real_path = get_real_path_for_doc_path (path, app_id);
          /* Try opening the directory via the file manager interface, then
             fall back to a plain URI open */
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GVariant) result = NULL;
          g_autoptr(GVariantBuilder) uris_builder = NULL;
          g_autofree char* item_uri = g_filename_to_uri (real_path, NULL, NULL);
          g_autoptr(GDBusConnection) bus = NULL;

          bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &local_error);

          uris_builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
          g_variant_builder_add (uris_builder, "s", item_uri);

          if (bus)
            result = g_dbus_connection_call_sync (bus,
                                                  FILE_MANAGER_DBUS_NAME,
                                                  FILE_MANAGER_DBUS_PATH,
                                                  FILE_MANAGER_DBUS_IFACE,
                                                  FILE_MANAGER_SHOW_ITEMS,
                                                  g_variant_new ("(ass)", uris_builder, ""),
                                                  NULL,   /* ignore returned type */
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  &local_error);

          if (result == NULL)
            {
              if (g_error_matches (local_error, G_DBUS_ERROR,
                                   G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
                  g_error_matches (local_error, G_DBUS_ERROR,
                                   G_DBUS_ERROR_SERVICE_UNKNOWN))
                g_debug ("No " FILE_MANAGER_DBUS_NAME " available");
              else
                g_warning ("Failed to call " FILE_MANAGER_SHOW_ITEMS ": %s",
                           local_error->message);
            }
          else
            {
              g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
              xdp_request_emit_response (XDP_REQUEST (request),
                                         XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                         g_variant_builder_end (&opts_builder));
              request_unexport (request);
              return;
            }

          g_free (path);
          path = g_path_get_dirname (real_path);
        }

      get_content_type_for_file (path, &content_type);
      basename = g_path_get_basename (path);

      scheme = g_strdup ("file");
      uri = g_filename_to_uri (path, NULL, NULL);
      g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (uri), g_free);
      close (fd);
      fd = -1;
      g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (-1));
    }

  g_object_set_data_full (G_OBJECT (request), "scheme", g_strdup (scheme), g_free);
  g_object_set_data_full (G_OBJECT (request), "content-type", g_strdup (content_type), g_free);

  /* collect all the information */
  find_recommended_choices (scheme, content_type, &default_app, &choices, &n_choices);
  /* it's never NULL, but might be empty (only contain the NULL terminator) */
  g_assert (choices != NULL);
  if (default_app != NULL && !app_exists (default_app))
    g_clear_pointer (&default_app, g_free);
  use_default_app = should_use_default_app (scheme, content_type);
  get_latest_choice_info (app_id, content_type,
                          &latest_id, &latest_count, &latest_threshold,
                          &ask_for_content_type);
  if (latest_id != NULL && !app_exists (latest_id))
    g_clear_pointer (&latest_id, g_free);

  skip_app_chooser = FALSE;
  reason = NULL;

  /* apply default handling: skip if the we have a default handler */
  if (default_app != NULL && use_default_app)
    {
      reason = "Allowing to skip app chooser: can use default";
      skip_app_chooser = TRUE;
    }

  if (n_choices == 1)
    {
      if (!skip_app_chooser)
        reason = "Allowing to skip app chooser: no choice";
      skip_app_chooser = TRUE;
    }

  /* also skip if the user has made the same choice often enough */
  if (latest_id != NULL && latest_count >= latest_threshold)
    {
      if (!skip_app_chooser)
        reason = "Allowing to skip app chooser: above threshold";
      skip_app_chooser = TRUE;
    }

  /* respect the app choices */
  if (ask)
    {
      if (skip_app_chooser)
        reason = "Refusing to skip app chooser: app request";
      skip_app_chooser = FALSE;
    }

  /* respect the users choices: paranoid mode overrides everything else */
  if (ask_for_content_type || latest_threshold == G_MAXINT)
    {
      if (skip_app_chooser)
        reason = "Refusing to skip app chooser: always-ask enabled";
      skip_app_chooser = FALSE;
    }

  g_debug ("%s", reason);

  if (skip_app_chooser)
    {
      const char *app = NULL;

      if (default_app != NULL && use_default_app)
        app = default_app;
      else if (latest_id != NULL)
        app = latest_id;
      else if (default_app != NULL)
        app = default_app;
      else if (n_choices > 0 && app_exists (choices[0]))
        app = choices[0];

      if (app)
        {
          /* Launch the app directly */
          g_autoptr(GError) error = NULL;

          g_debug ("Skipping app chooser");

          gboolean result = launch_application_with_uri (app, uri, parent_window, writable, activation_token, &error);
          if (request->exported)
            {
              if (!result)
                g_debug ("Open request for '%s' failed: %s", uri, error->message);
              g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
              xdp_request_emit_response (XDP_REQUEST (request),
                                         result ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS : XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                         g_variant_builder_end (&opts_builder));
              request_unexport (request);
            }

          return;
        }
    }

  g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);

  if (latest_id != NULL)
    g_variant_builder_add (&opts_builder, "{sv}", "last_choice", g_variant_new_string (latest_id));
  else if (default_app != NULL)
    g_variant_builder_add (&opts_builder, "{sv}", "last_choice", g_variant_new_string (default_app));

  g_object_set_data_full (G_OBJECT (request), "content-type", g_strdup (content_type), g_free);

  g_variant_builder_add (&opts_builder, "{sv}", "content_type", g_variant_new_string (content_type));
  if (basename)
    g_variant_builder_add (&opts_builder, "{sv}", "filename", g_variant_new_string (basename));
  if (uri)
    g_variant_builder_add (&opts_builder, "{sv}", "uri", g_variant_new_string (uri));
  if (activation_token)
    g_variant_builder_add (&opts_builder, "{sv}", "activation_token", g_variant_new_string (uri));

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, NULL);

  request_set_impl_request (request, impl_request);

  g_signal_connect_object (monitor, "changed", G_CALLBACK (app_info_changed), request, 0);

  g_debug ("Opening app chooser");

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
  gboolean ask;
  const char *activation_token = NULL;

  if (xdp_impl_lockdown_get_disable_application_handlers (lockdown))
    {
      g_debug ("Application handlers disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Application handlers disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!g_variant_lookup (arg_options, "writable", "b", &writable))
    writable = FALSE;

  if (!g_variant_lookup (arg_options, "ask", "b", &ask))
    ask = FALSE;

  g_variant_lookup (arg_options, "activation_token", "&s", &activation_token);

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (-1));
  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data (G_OBJECT (request), "writable", GINT_TO_POINTER (writable));
  g_object_set_data (G_OBJECT (request), "ask", GINT_TO_POINTER (ask));

  if (activation_token)
    g_object_set_data_full (G_OBJECT (request), "activation-token", g_strdup (activation_token), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_open_uri_complete_open_uri (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_open_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_open_file (XdpOpenURI *object,
                 GDBusMethodInvocation *invocation,
                 GUnixFDList *fd_list,
                 const gchar *arg_parent_window,
                 GVariant *arg_fd,
                 GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  gboolean writable;
  gboolean ask;
  int fd_id, fd;
  const char *activation_token = NULL;
  g_autoptr(GError) error = NULL;

  if (xdp_impl_lockdown_get_disable_application_handlers (lockdown))
    {
      g_debug ("Application handlers disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Application handlers disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!g_variant_lookup (arg_options, "writable", "b", &writable))
    writable = FALSE;

  if (!g_variant_lookup (arg_options, "ask", "b", &ask))
    ask = FALSE;

  g_variant_get (arg_fd, "h", &fd_id);
  fd = g_unix_fd_list_get (fd_list, fd_id, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_lookup (arg_options, "activation_token", "&s", &activation_token);

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (fd));
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data (G_OBJECT (request), "writable", GINT_TO_POINTER (writable));
  g_object_set_data (G_OBJECT (request), "ask", GINT_TO_POINTER (ask));

  if (activation_token)
    g_object_set_data_full (G_OBJECT (request), "activation-token", g_strdup (activation_token), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_open_uri_complete_open_file (object, invocation, NULL, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_open_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_open_directory (XdpOpenURI *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList *fd_list,
                       const gchar *arg_parent_window,
                       GVariant *arg_fd,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  int fd_id, fd;
  const char *activation_token = NULL;
  g_autoptr(GError) error = NULL;

  if (xdp_impl_lockdown_get_disable_application_handlers (lockdown))
    {
      g_debug ("Application handlers disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Application handlers disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_get (arg_fd, "h", &fd_id);
  fd = g_unix_fd_list_get (fd_list, fd_id, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_lookup (arg_options, "activation_token", "&s", &activation_token);

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (fd));
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data (G_OBJECT (request), "writable", GINT_TO_POINTER (0));
  g_object_set_data (G_OBJECT (request), "ask", GINT_TO_POINTER (0));
  g_object_set_data (G_OBJECT (request), "open-dir", GINT_TO_POINTER (1));

  if (activation_token)
    g_object_set_data_full (G_OBJECT (request), "activation-token", g_strdup (activation_token), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_open_uri_complete_open_file (object, invocation, NULL, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_open_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
open_uri_iface_init (XdpOpenURIIface *iface)
{
  iface->handle_open_uri = handle_open_uri;
  iface->handle_open_file = handle_open_file;
  iface->handle_open_directory = handle_open_directory;
}

static void
open_uri_init (OpenURI *openuri)
{
  xdp_open_uri_set_version (XDP_OPEN_URI (openuri), 3);
}

static void
open_uri_class_init (OpenURIClass *klass)
{
}

GDBusInterfaceSkeleton *
open_uri_create (GDBusConnection *connection,
                 const char *dbus_name,
                 gpointer lockdown_proxy)
{
  g_autoptr(GError) error = NULL;

  lockdown = lockdown_proxy;

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

  monitor = g_app_info_monitor_get ();

  return G_DBUS_INTERFACE_SKELETON (open_uri);
}

