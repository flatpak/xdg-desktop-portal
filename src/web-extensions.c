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
#include <gio/gunixfdlist.h>
#include <json-glib/json-glib.h>

#include "xdp-session.h"
#include "web-extensions.h"
#include "xdp-request.h"
#include "xdp-permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

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

static WebExtensions *web_extensions;

GType web_extensions_get_type (void);
static void web_extensions_iface_init (XdpDbusWebExtensionsIface *iface);

G_DEFINE_TYPE_WITH_CODE (WebExtensions, web_extensions, XDP_DBUS_TYPE_WEB_EXTENSIONS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WEB_EXTENSIONS,
                                                web_extensions_iface_init));

typedef enum _WebExtensionsSessionState
{
  WEB_EXTENSIONS_SESSION_STATE_INIT,
  WEB_EXTENSIONS_SESSION_STATE_STARTED,
  WEB_EXTENSIONS_SESSION_STATE_CLOSED,
} WebExtensionsSessionState;

typedef struct _WebExtensionsSession
{
  XdpSession parent;

  WebExtensionsSessionState state;

  GPid child_pid;
  guint child_watch_id;

  int standard_input;
  int standard_output;
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
}

static void
web_extensions_session_close (XdpSession *session)
{
  WebExtensionsSession *we_session = (WebExtensionsSession *)session;

  if (we_session->state == WEB_EXTENSIONS_SESSION_STATE_CLOSED) return;

  we_session->state = WEB_EXTENSIONS_SESSION_STATE_CLOSED;
  if (we_session->child_watch_id != 0)
    {
      g_source_remove (we_session->child_watch_id);
      we_session->child_watch_id = 0;
    }

  if (we_session->child_pid > 0)
    {
      kill (we_session->child_pid, SIGTERM);
      waitpid (we_session->child_pid, NULL, 0);
      g_spawn_close_pid (we_session->child_pid);
      we_session->child_pid = -1;
    }

  if (we_session->standard_input >= 0)
    {
      close (we_session->standard_input);
      we_session->standard_input = -1;
    }
  if (we_session->standard_output >= 0)
    {
      close (we_session->standard_output);
      we_session->standard_output = -1;
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
  const char *session_token;

  session_token = lookup_session_token (options);
  session = g_initable_new (web_extensions_session_get_type (), NULL, error,
                            "sender", call->sender,
                            "app-id", xdp_app_info_get_id (call->app_info),
                            "token", session_token,
                            "connection", connection,
                            NULL);

  if (session)
    g_debug ("screen cast session owned by '%s' created", session->sender);

  return (WebExtensionsSession *)session;
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
on_server_exited (GPid pid,
                  gint status,
                  gpointer user_data)
{
  WebExtensionsSession *we_session = user_data;

  we_session->child_pid = -1;
  we_session->child_watch_id = 0;
  xdp_session_close ((XdpSession *)we_session, TRUE);
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
  return g_regex_match_simple ("^\\w+(\\.\\w+)*$", name, 0, 0);
}

static char *
find_server (const char *server_name,
             const char *extension_or_origin,
             GError **error)
{
  const char *hosts_path_str;
  g_auto(GStrv) hosts_path;
  g_autoptr(JsonParser) parser = NULL;
  g_autofree char *metadata_basename = NULL;
  int i;

  /* Check that the we have a valid native messaging host name */
  if (!is_valid_name (server_name))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT, "invalid native messaging server name");
      return NULL;
    }

  hosts_path_str = g_getenv ("XDG_DESKTOP_PORTAL_WEB_EXTENSIONS_PATH");
  if (hosts_path_str == NULL)
    {
      hosts_path_str = "/usr/lib/mozilla/native-messaging-hosts:/etc/opt/chrome/native-messaging-hosts:/etc/chromium/native-messaging-hosts";
    }

  hosts_path = g_strsplit (hosts_path_str, ":", -1);
  parser = json_parser_new ();
  metadata_basename = g_strconcat (server_name, ".json", NULL);

  for (i = 0; hosts_path[i] != NULL; i++)
    {
      g_autofree char *metadata_filename = NULL;
      JsonObject *metadata_root;

      metadata_filename = g_build_filename (hosts_path[i], metadata_basename, NULL);
      if (!g_file_test (metadata_filename, G_FILE_TEST_EXISTS))
        continue;

      if (!json_parser_load_from_file (parser, metadata_filename, error))
        return NULL;

      metadata_root = json_node_get_object (json_parser_get_root (parser));

      /* Skip if metadata contains an unexpected name */
      if (g_strcmp0 (json_object_get_string_member (metadata_root, "name"), server_name) != 0)
        continue;

      /* Skip if this is not a "stdio" type native messaging server */
      if (g_strcmp0 (json_object_get_string_member (metadata_root, "type"), "stdio") != 0)
        continue;

      /* Skip if this server isn't available to the extension. Note
       * that this ID is provided by the sandboxed browser, so this
       * check is just to help implement its security policy. */
      if (!array_contains (json_object_get_array_member (metadata_root, "allowed_extensions"), extension_or_origin) &&
          !array_contains (json_object_get_array_member (metadata_root, "allowed_origins"), extension_or_origin))
        continue;

      /* Server matches: return its executable path */
      return g_strdup (json_object_get_string_member (metadata_root, "path"));
    }

  g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND, "cannot find native messaging server");
  return NULL;
}

static gboolean
handle_start (XdpDbusWebExtensions *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_name,
              const char *arg_extension_or_origin,
              GVariant *arg_options)
{
  XdpCall *call = xdp_call_from_invocation (invocation);
  XdpSession *session;
  WebExtensionsSession *we_session;
  g_autofree char *server_path = NULL;
  char *argv[] = {NULL, NULL};
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
  we_session = (WebExtensionsSession *)session;

  if (we_session->state != WEB_EXTENSIONS_SESSION_STATE_INIT)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session already started");
      return TRUE;
    }

  server_path = find_server (arg_name, arg_extension_or_origin, &error);
  if (server_path == NULL)
    {
      xdp_session_close(session, TRUE);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  argv[0] = server_path;
  if (!g_spawn_async_with_pipes (NULL, /* working_directory */
                                 argv,
                                 NULL, /* envp */
                                 G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, /* child_setup */
                                 NULL, /* user_data */
                                 &we_session->child_pid,
                                 &we_session->standard_input,
                                 &we_session->standard_output,
                                 NULL, /* standard_error */
                                 &error))
    {
      we_session->child_pid = -1;
      xdp_session_close(session, TRUE);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  we_session->child_watch_id = g_child_watch_add (
    we_session->child_pid, on_server_exited, we_session);
  we_session->state = WEB_EXTENSIONS_SESSION_STATE_STARTED;

  xdp_dbus_web_extensions_complete_start (object, invocation);

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
  WebExtensionsSession *we_session;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int stdin_id, stdout_id;
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
  we_session = (WebExtensionsSession *)session;

  if (we_session->state != WEB_EXTENSIONS_SESSION_STATE_STARTED)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session not started");
      return TRUE;
    }

  out_fd_list = g_unix_fd_list_new ();
  stdin_id = g_unix_fd_list_append (
    out_fd_list, we_session->standard_input, &error);
  if (stdin_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Failed to append fd: %s",
                                             error->message);
      return TRUE;
    }

  stdout_id = g_unix_fd_list_append (
    out_fd_list, we_session->standard_output, &error);
  if (stdout_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Failed to append fd: %s",
                                             error->message);
      return TRUE;
    }

  xdp_dbus_web_extensions_complete_get_pipes (object, invocation, out_fd_list,
                                              g_variant_new_handle (stdin_id),
                                              g_variant_new_handle (stdout_id));
  return TRUE;
}

static void
web_extensions_iface_init (XdpDbusWebExtensionsIface *iface)
{
  iface->handle_create_session = handle_create_session;
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
web_extensions_create (GDBusConnection *connection)
{
  web_extensions = g_object_new (web_extensions_get_type (), NULL);
  return G_DBUS_INTERFACE_SKELETON (web_extensions);
}
