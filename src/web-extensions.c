/*
 * Copyright © 2022 Canonical Ltd
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
 */

#include "config.h"

#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>
#include <json-glib/json-glib.h>

#include "xdp-session.h"
#include "web-extensions.h"
#include "xdp-request.h"
#include "xdp-permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "webextensions"

typedef struct _WebExtensions WebExtensions;
typedef struct _WebExtensionsClass WebExtensionsClass;

struct _WebExtensions
{
  XdpDbusWebExtensionsSkeleton parent_instance;
};

struct _WebExtensionsClass
{
  XdpDbusWebExtensionsSkeletonClass parent_class;
};

static XdpDbusImplAccess *access_impl;
static WebExtensions *web_extensions;

GType web_extensions_get_type (void);
static void web_extensions_iface_init (XdpDbusWebExtensionsIface *iface);

G_DEFINE_TYPE_WITH_CODE (WebExtensions, web_extensions, XDP_DBUS_TYPE_WEB_EXTENSIONS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WEB_EXTENSIONS,
                                                web_extensions_iface_init));

typedef enum _WebExtensionsSessionMode
{
  WEB_EXTENSIONS_SESSION_MODE_CHROMIUM,
  WEB_EXTENSIONS_SESSION_MODE_MOZILLA,
} WebExtensionsSessionMode;

typedef enum _WebExtensionsSessionState
{
  WEB_EXTENSIONS_SESSION_STATE_INIT,
  WEB_EXTENSIONS_SESSION_STATE_STARTING,
  WEB_EXTENSIONS_SESSION_STATE_STARTED,
  WEB_EXTENSIONS_SESSION_STATE_CLOSED,
} WebExtensionsSessionState;

typedef struct _WebExtensionsSession
{
  XdpSession parent;

  WebExtensionsSessionMode mode;
  WebExtensionsSessionState state;

  GPid child_pid;
  guint child_watch_id;

  int standard_input;
  int standard_output;
  int standard_error;
} WebExtensionsSession;

typedef struct _WebExtensionsSessionClass
{
  XdpSessionClass parent_class;
} WebExtensionsSessionClass;

GType web_extensions_session_get_type (void);

G_DEFINE_TYPE (WebExtensionsSession, web_extensions_session, xdp_session_get_type ());

static void
web_extensions_session_init (WebExtensionsSession *session)
{
  session->child_pid = -1;
  session->child_watch_id = 0;

  session->standard_input = -1;
  session->standard_output = -1;
  session->standard_error = -1;
}

static void
web_extensions_session_close (XdpSession *session)
{
  WebExtensionsSession *web_extensions_session = (WebExtensionsSession *)session;

  if (web_extensions_session->state == WEB_EXTENSIONS_SESSION_STATE_CLOSED) return;

  web_extensions_session->state = WEB_EXTENSIONS_SESSION_STATE_CLOSED;
  if (web_extensions_session->child_watch_id != 0)
    {
      g_source_remove (web_extensions_session->child_watch_id);
      web_extensions_session->child_watch_id = 0;
    }

  if (web_extensions_session->child_pid > 0)
    {
      kill (web_extensions_session->child_pid, SIGKILL);
      waitpid (web_extensions_session->child_pid, NULL, 0);
      g_spawn_close_pid (web_extensions_session->child_pid);
      web_extensions_session->child_pid = -1;
    }

  if (web_extensions_session->standard_input >= 0)
    {
      close (web_extensions_session->standard_input);
      web_extensions_session->standard_input = -1;
    }
  if (web_extensions_session->standard_output >= 0)
    {
      close (web_extensions_session->standard_output);
      web_extensions_session->standard_output = -1;
    }
  if (web_extensions_session->standard_error >= 0)
    {
      close (web_extensions_session->standard_error);
      web_extensions_session->standard_error = -1;
    }
}

static void
web_extensions_session_finalize (GObject *object)
{
  XdpSession *session = (XdpSession *)object;

  web_extensions_session_close (session);
  G_OBJECT_CLASS (web_extensions_session_parent_class)->finalize (object);
}

static void
web_extensions_session_class_init (WebExtensionsSessionClass *klass)
{
  GObjectClass *object_class;
  XdpSessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = web_extensions_session_finalize;

  session_class = (XdpSessionClass *)klass;
  session_class->close = web_extensions_session_close;
}

static WebExtensionsSession *
web_extensions_session_new (GVariant *options,
                            XdpCall *call,
                            GDBusConnection *connection,
                            GError **error)
{
  XdpSession *session;
  WebExtensionsSession *web_extensions_session;
  WebExtensionsSessionMode mode = WEB_EXTENSIONS_SESSION_MODE_MOZILLA;
  const char *mode_str = NULL;
  const char *session_token;

  g_variant_lookup (options, "mode", "&s", &mode_str);
  if (mode_str != NULL)
    {
      if (!strcmp(mode_str, "chromium"))
        mode = WEB_EXTENSIONS_SESSION_MODE_CHROMIUM;
      else if (!strcmp(mode_str, "mozilla"))
        mode = WEB_EXTENSIONS_SESSION_MODE_MOZILLA;
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Invalid mode");
          return NULL;
        }
    }

  session_token = lookup_session_token (options);
  session = g_initable_new (web_extensions_session_get_type (), NULL, error,
                            "sender", call->sender,
                            "app-id", xdp_app_info_get_id (call->app_info),
                            "token", session_token,
                            "connection", connection,
                            NULL);

  if (session)
    g_debug ("webextensions session owned by '%s' created", session->sender);

  web_extensions_session = (WebExtensionsSession *)session;
  web_extensions_session->mode = mode;
  return web_extensions_session;
}

static gboolean
handle_create_session (XdpDbusWebExtensions *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  XdpCall *call = xdp_call_from_invocation (invocation);
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(GError) error = NULL;
  XdpSession *session;

  session = (XdpSession *)web_extensions_session_new (arg_options, call, connection, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (!xdp_session_export (session, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      xdp_session_close (session, FALSE);
      return TRUE;
    }
  xdp_session_register (session);

  xdp_dbus_web_extensions_complete_create_session (object, invocation, session->id);

  return TRUE;
}

static void
on_host_exited (GPid pid,
                gint status,
                gpointer user_data)
{
  XdpSession *session = user_data;
  WebExtensionsSession *web_extensions_session = (WebExtensionsSession *)session;

  SESSION_AUTOLOCK (session);
  web_extensions_session->child_pid = -1;
  web_extensions_session->child_watch_id = 0;
  xdp_session_close (session, TRUE);
}

static gboolean
array_contains (JsonArray *array,
                const char *value)
{
  guint length, i;

  if (array == NULL)
    return FALSE;

  length = json_array_get_length (array);
  for (i = 0; i < length; i++)
    {
      const char *element = json_array_get_string_element (array, i);
      if (g_strcmp0 (element, value) == 0)
        return TRUE;
    }
  return FALSE;
}

static gboolean
is_valid_name (const char *name)
{
  /* This regexp comes from the Mozilla documentation on valid native
     messaging host names:

     https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_manifests#native_messaging_manifests

     That is, one or more dot-separated groups composed of
     alphanumeric characters and underscores.
  */
  return g_regex_match_simple ("^\\w+(\\.\\w+)*$", name, 0, 0);
}

static GStrv
get_manifest_search_path (WebExtensionsSessionMode mode)
{
  const char *hosts_path_str;
  g_autoptr(GPtrArray) search_path = NULL;

  hosts_path_str = g_getenv ("XDG_DESKTOP_PORTAL_WEB_EXTENSIONS_PATH");
  if (hosts_path_str != NULL)
    return g_strsplit (hosts_path_str, ":", -1);

  search_path = g_ptr_array_new_with_free_func (g_free);
  switch (mode)
    {
    case WEB_EXTENSIONS_SESSION_MODE_CHROMIUM:
      /* Chrome and Chromium search paths documented here:
       * https://developer.chrome.com/docs/apps/nativeMessaging/#native-messaging-host-location
       */
      /* Add per-user directories */
      g_ptr_array_add (search_path, g_build_filename (g_get_user_config_dir (), "google-chrome", "NativeMessagingHosts", NULL));
      g_ptr_array_add (search_path, g_build_filename (g_get_user_config_dir (), "chromium", "NativeMessagingHosts", NULL));
      /* Add system wide directories */
      g_ptr_array_add (search_path, g_strdup ("/etc/opt/chrome/native-messaging-hosts"));
      g_ptr_array_add (search_path, g_strdup ("/etc/chromium/native-messaging-hosts"));
      /* And the same for xdg-desktop-portal's configured prefix */
      g_ptr_array_add (search_path, g_strdup (SYSCONFDIR "opt/chrome/native-messaging-hosts"));
      g_ptr_array_add (search_path, g_strdup (SYSCONFDIR "chromium/native-messaging-hosts"));
      break;

    case WEB_EXTENSIONS_SESSION_MODE_MOZILLA:
      /* Firefox search paths documented here:
       * https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_manifests#manifest_location
       */
      /* Add per-user directories */
      g_ptr_array_add (search_path, g_build_filename (g_get_home_dir (), ".mozilla", "native-messaging-hosts", NULL));
      /* Add system wide directories */
      g_ptr_array_add (search_path, g_strdup ("/usr/lib/mozilla/native-messaging-hosts"));
      g_ptr_array_add (search_path, g_strdup ("/usr/lib64/mozilla/native-messaging-hosts"));
      /* And the same for xdg-desktop-portal's configured prefix */
      g_ptr_array_add (search_path, g_strdup (LIBDIR "mozilla/native-messaging-hosts"));
      break;
    }

  g_ptr_array_add (search_path, NULL);
  return (GStrv)g_ptr_array_free (g_steal_pointer (&search_path), FALSE);
}

static char *
find_messaging_host (WebExtensionsSessionMode mode,
                     const char *messaging_host_name,
                     const char *extension_or_origin,
                     char **out_description,
                     char **out_manifest_filename,
                     char **out_json_manifest,
                     GError **error)
{
  g_auto(GStrv) search_path = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autofree char *metadata_basename = NULL;
  int i;

  /* Check that the we have a valid native messaging host name */
  if (!is_valid_name (messaging_host_name))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid native messaging host name");
      return NULL;
    }

  search_path = get_manifest_search_path (mode);
  parser = json_parser_new ();
  metadata_basename = g_strconcat (messaging_host_name, ".json", NULL);

  for (i = 0; search_path[i] != NULL; i++)
    {
      g_autofree char *metadata_filename = NULL;
      g_autoptr(GError) load_error = NULL;
      JsonObject *metadata_root;
      const char *host_path;

      metadata_filename = g_build_filename (search_path[i], metadata_basename, NULL);
      if (!json_parser_load_from_file (parser, metadata_filename, &load_error))
        {
          /* If the file doesn't exist, continue searching. Error out
             on anything else. */
          if (g_error_matches (load_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            continue;
          g_propagate_error (error, g_steal_pointer (&load_error));
          return NULL;
        }

      metadata_root = json_node_get_object (json_parser_get_root (parser));

      /* Skip if metadata contains an unexpected name */
      if (g_strcmp0 (json_object_get_string_member (metadata_root, "name"), messaging_host_name) != 0)
        continue;

      /* Skip if this is not a "stdio" type native messaging host */
      if (g_strcmp0 (json_object_get_string_member (metadata_root, "type"), "stdio") != 0)
        continue;

      /* Skip if this host isn't available to the extension. Note
       * that this ID is provided by the sandboxed browser, so this
       * check is just to help implement its security policy. */
      switch (mode)
        {
        case WEB_EXTENSIONS_SESSION_MODE_CHROMIUM:
          if (!array_contains (json_object_get_array_member (metadata_root, "allowed_origins"), extension_or_origin))
            continue;
          break;
        case WEB_EXTENSIONS_SESSION_MODE_MOZILLA:
          if (!array_contains (json_object_get_array_member (metadata_root, "allowed_extensions"), extension_or_origin))
            continue;
          break;
        }

      host_path = json_object_get_string_member (metadata_root, "path");
      if (!g_path_is_absolute (host_path))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_FAILED,
                       "Native messaging host path is not absolute");
          return NULL;
        }

      /* Host matches: return its executable path and description */
      if (out_description != NULL)
        *out_description = g_strdup (json_object_get_string_member (metadata_root, "description"));
      if (out_manifest_filename != NULL)
        *out_manifest_filename = g_steal_pointer (&metadata_filename);
      if (out_json_manifest != NULL)
        *out_json_manifest = json_to_string (json_parser_get_root (parser), FALSE);
      return g_strdup (host_path);
    }

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
               "Could not find native messaging host");
  return NULL;
}

static gboolean
handle_get_manifest (XdpDbusWebExtensions *object,
                     GDBusMethodInvocation *invocation,
                     const char *arg_session_handle,
                     const char *arg_name,
                     const char *arg_extension_or_origin)
{
  XdpCall *call = xdp_call_from_invocation (invocation);
  XdpSession *session;
  WebExtensionsSession *web_extensions_session;
  g_autofree char *host_path = NULL;
  g_autofree char *json_manifest = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);
  web_extensions_session = (WebExtensionsSession *)session;

  if (web_extensions_session->state != WEB_EXTENSIONS_SESSION_STATE_INIT)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session already started");
      return TRUE;
    }

  host_path = find_messaging_host (web_extensions_session->mode,
                                   arg_name, arg_extension_or_origin,
                                   NULL, NULL, &json_manifest, &error);
  if (!host_path)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  xdp_dbus_web_extensions_complete_get_manifest (object, invocation, json_manifest);
  return TRUE;
}

static void
handle_start_in_thread (GTask *task,
                        gpointer source_object,
                        gpointer task_data,
                        GCancellable *cancellable)
{
  XdpRequest *request = (XdpRequest *)task_data;
  XdpSession *session;
  WebExtensionsSession *web_extensions_session;
  const char *arg_name;
  char *arg_extension_or_origin;
  const char *app_id;
  g_autofree char *host_path = NULL;
  g_autofree char *description = NULL;
  g_autofree char *manifest_filename = NULL;
  guint response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  gboolean should_close_session;
  XdpPermission permission;
  gboolean allowed;
  char *argv[] = {NULL, NULL, NULL, NULL};
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);
  session = g_object_get_data (G_OBJECT (request), "session");
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_data (G_OBJECT (request), "session", NULL);
  web_extensions_session = (WebExtensionsSession *)session;

  if (!request->exported || web_extensions_session->state != WEB_EXTENSIONS_SESSION_STATE_STARTING)
    goto out;

  arg_name = g_object_get_data (G_OBJECT (request), "name");
  arg_extension_or_origin = g_object_get_data (G_OBJECT (request), "extension-or-origin");

  host_path = find_messaging_host (web_extensions_session->mode,
                                   arg_name, arg_extension_or_origin,
                                   &description, &manifest_filename, NULL,
                                   &error);
  if (host_path == NULL)
    {
      g_warning ("Could not find WebExtensions backend: %s", error->message);
      fflush(stderr);
      fflush(stdout);
      goto out;
    }

  app_id = xdp_app_info_get_id (request->app_info);
  permission = xdp_get_permission_sync (app_id, PERMISSION_TABLE, arg_name);
  if (permission == XDP_PERMISSION_ASK || permission == XDP_PERMISSION_UNSET)
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      GVariantBuilder opt_builder;
      GAppInfo *info = NULL;
      const char *display_name;
      g_autofree gchar *app_info_id = NULL;
      g_autofree gchar *title = NULL;
      g_autofree gchar *subtitle = NULL;
      g_autofree gchar *body = NULL;

      info = xdp_app_info_get_gappinfo (request->app_info);
      if (info)
        {
          g_auto(GStrv) app_id_components = g_strsplit (g_app_info_get_id (info), ".desktop", 2);
          app_info_id = g_strdup (app_id_components[0]);
        }
      display_name = info ? g_app_info_get_display_name (info) : app_id;
      title = g_strdup_printf (_("Allow %s to start WebExtension backend?"), display_name);
      subtitle = g_strdup_printf (_("%s is requesting to launch \"%s\" (%s)."), display_name, description, arg_name);
      body = g_strdup (_("This permission can be changed at any time from the privacy settings."));

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&opt_builder, "{sv}", "deny_label", g_variant_new_string (_("Don't allow")));
      g_variant_builder_add (&opt_builder, "{sv}", "grant_label", g_variant_new_string (_("Allow")));
      if (!xdp_dbus_impl_access_call_access_dialog_sync (access_impl,
                                                         request->id,
                                                         app_info_id ? app_info_id : app_id,
                                                         "",
                                                         title,
                                                         subtitle,
                                                         body,
                                                         g_variant_builder_end (&opt_builder),
                                                         &access_response,
                                                         &access_results,
                                                         NULL,
                                                         &error))
        {
          g_warning ("AccessDialog call failed: %s", error->message);
          g_clear_error (&error);
        }
      allowed = access_response == 0;

      if (permission == XDP_PERMISSION_UNSET)
        xdp_set_permission_sync (app_id, PERMISSION_TABLE, arg_name, allowed ? XDP_PERMISSION_YES : XDP_PERMISSION_NO);
    }
  else
    {
      allowed = permission == XDP_PERMISSION_YES ? TRUE : FALSE;
    }

  if (!allowed)
    {
      response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      goto out;
    }

  argv[0] = host_path;
  switch (web_extensions_session->mode)
    {
    case WEB_EXTENSIONS_SESSION_MODE_CHROMIUM:
      argv[1] = arg_extension_or_origin;
      break;
    case WEB_EXTENSIONS_SESSION_MODE_MOZILLA:
      argv[1] = manifest_filename;
      argv[2] = arg_extension_or_origin;
      break;
    }
  if (!g_spawn_async_with_pipes (NULL, /* working_directory */
                                 argv,
                                 NULL, /* envp */
                                 G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, /* child_setup */
                                 NULL, /* user_data */
                                 &web_extensions_session->child_pid,
                                 &web_extensions_session->standard_input,
                                 &web_extensions_session->standard_output,
                                 &web_extensions_session->standard_error,
                                 &error))
    {
      web_extensions_session->child_pid = -1;
      goto out;
    }

  web_extensions_session->child_watch_id = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                                   web_extensions_session->child_pid,
                                                                   on_host_exited,
                                                                   g_object_ref (web_extensions_session),
                                                                   g_object_unref);
  web_extensions_session->state = WEB_EXTENSIONS_SESSION_STATE_STARTED;

  response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;

out:
  should_close_session = !request->exported || response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, g_variant_builder_end (&results));
      xdp_request_unexport (request);
    }

  if (should_close_session)
    xdp_session_close (session, TRUE);
}

static gboolean
handle_start (XdpDbusWebExtensions *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_name,
              const char *arg_extension_or_origin,
              GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  WebExtensionsSession *web_extensions_session;
  g_autoptr(GTask) task = NULL;

  REQUEST_AUTOLOCK (request);

  session = xdp_session_from_request (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);
  web_extensions_session = (WebExtensionsSession *)session;

  if (web_extensions_session->state != WEB_EXTENSIONS_SESSION_STATE_INIT)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session already started");
      return TRUE;
    }

  web_extensions_session->state = WEB_EXTENSIONS_SESSION_STATE_STARTING;
  g_object_set_data_full (G_OBJECT (request), "session", g_object_ref (session), g_object_unref);
  g_object_set_data_full (G_OBJECT (request), "name", g_strdup (arg_name), g_free);
  g_object_set_data_full (G_OBJECT (request), "extension-or-origin", g_strdup (arg_extension_or_origin), g_free);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_web_extensions_complete_start (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_start_in_thread);

  return TRUE;
}


static gboolean
handle_get_pipes (XdpDbusWebExtensions *object,
                  GDBusMethodInvocation *invocation,
                  GUnixFDList *fd_list,
                  const char *arg_session_handle,
                  GVariant *arg_options)
{
  XdpCall *call = xdp_call_from_invocation (invocation);
  XdpSession *session;
  WebExtensionsSession *web_extensions_session;
  int fds[3];
  g_autoptr(GUnixFDList) out_fd_list = NULL;

  session = xdp_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);
  web_extensions_session = (WebExtensionsSession *)session;

  if (web_extensions_session->state != WEB_EXTENSIONS_SESSION_STATE_STARTED)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session not started");
      return TRUE;
    }

  if (web_extensions_session->standard_input < 0 ||
      web_extensions_session->standard_output < 0 ||
      web_extensions_session->standard_error < 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "GetPipes already called");
      return TRUE;
    }

  fds[0] = web_extensions_session->standard_input;
  fds[1] = web_extensions_session->standard_output;
  fds[2] = web_extensions_session->standard_error;
  out_fd_list = g_unix_fd_list_new_from_array (fds, G_N_ELEMENTS (fds));
  /* out_fd_list now owns the file descriptors */
  web_extensions_session->standard_input = -1;
  web_extensions_session->standard_output = -1;
  web_extensions_session->standard_error = -1;

  xdp_dbus_web_extensions_complete_get_pipes (object, invocation, out_fd_list,
                                              g_variant_new_handle (0),
                                              g_variant_new_handle (1),
                                              g_variant_new_handle (2));
  return TRUE;
}

static void
web_extensions_iface_init (XdpDbusWebExtensionsIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_get_manifest = handle_get_manifest;
  iface->handle_start = handle_start;
  iface->handle_get_pipes = handle_get_pipes;
}

static void
web_extensions_init (WebExtensions *web_extensions)
{
  xdp_dbus_web_extensions_set_version (XDP_DBUS_WEB_EXTENSIONS (web_extensions), 1);
}

static void
web_extensions_class_init (WebExtensionsClass *klass)
{
}

GDBusInterfaceSkeleton *
web_extensions_create (GDBusConnection *connection,
                       const char *dbus_name_access)
{
  g_autoptr(GError) error = NULL;

  web_extensions = g_object_new (web_extensions_get_type (), NULL);

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name_access,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);

  return G_DBUS_INTERFACE_SKELETON (web_extensions);
}
