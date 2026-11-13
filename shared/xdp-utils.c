/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixoutputstream.h>
#include <json-glib/json-glib.h>
#include <sys/ioctl.h>
#include <sys/random.h>

#include "xdp-types.h"


static gboolean
needs_quoting (const char *arg)
{
  while (*arg != 0)
    {
      char c = *arg;
      if (!g_ascii_isalnum (c) &&
          !(c == '-' || c == '/' || c == '~' ||
            c == ':' || c == '.' || c == '_' ||
            c == '=' || c == '@'))
        return TRUE;
      arg++;
    }
  return FALSE;
}

typedef struct
{
  XdpPeerDisconnectCallback peer_disconnect_cb;
  gpointer user_data;
} PeerDisconnectData;

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  PeerDisconnectData *data = user_data;
  const char *name, *from, *to;

  if (!data->peer_disconnect_cb)
    return;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] != ':' ||
      g_strcmp0 (name, from) != 0 ||
      g_strcmp0 (to, "") != 0)
    return;

  data->peer_disconnect_cb (name, data->user_data);
}

guint
xdp_connection_track_peer_disconnect (GDBusConnection           *connection,
                                      XdpPeerDisconnectCallback  peer_disconnect_cb,
                                      gpointer                   user_data)
{
  PeerDisconnectData *data = g_new0 (PeerDisconnectData, 1);

  data->peer_disconnect_cb = peer_disconnect_cb;
  data->user_data = user_data;

  return g_dbus_connection_signal_subscribe (connection,
                                             DBUS_DBUS_NAME,
                                             DBUS_DBUS_IFACE,
                                             "NameOwnerChanged",
                                             DBUS_DBUS_PATH,
                                             NULL,
                                             G_DBUS_SIGNAL_FLAGS_NONE,
                                             name_owner_changed,
                                             g_steal_pointer (&data),
                                             g_free);
}

void
xdp_connection_untrack_peer_disconnect (GDBusConnection *connection,
                                        guint            subscription_id)
{
  g_dbus_connection_signal_unsubscribe (connection, subscription_id);
}

static GLnxPidRef *
connection_get_peer_pidref_legacy_fiber (GDBusConnection *connection,
                                         const char      *sender,
                                         GError         **error)
{
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GVariant) reply = NULL;
  uint32_t pid;

  future = dex_dbus_connection_call (connection,
                                     DBUS_DBUS_NAME,
                                     DBUS_DBUS_PATH,
                                     DBUS_DBUS_IFACE,
                                     "GetConnectionUnixProcessID",
                                     g_variant_new ("(s)", sender),
                                     G_VARIANT_TYPE ("(u)"),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     30000);

  reply = dex_await_variant (g_steal_pointer (&future), error);
  if (!reply)
    return NULL;

  g_variant_get (reply, "(u)", &pid);

  return glnx_pid_ref_new_from_pid (pid, error);
}

static DexFuture *
connection_get_peer_pidref_fiber (GDBusConnection *connection,
                                  const char      *sender)
{
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) dict = NULL;
  g_autoptr(GVariant) process_fd = NULL;
  g_autoptr(GVariant) process_id = NULL;
  g_autoptr(GLnxPidRef) pidref = NULL;
  int fd_id;
  uint32_t pid;
  g_autofd int pidfd = -1;

  future = dex_dbus_connection_call_with_unix_fd_list (connection,
                                                       DBUS_DBUS_NAME,
                                                       DBUS_DBUS_PATH,
                                                       DBUS_DBUS_IFACE,
                                                       "GetConnectionCredentials",
                                                       g_variant_new ("(s)", sender),
                                                       G_VARIANT_TYPE ("(a{sv})"),
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       30000,
                                                       NULL);

  if (!dex_await (dex_ref (future), &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE))
        {
          g_clear_error (&local_error);
          pidref = connection_get_peer_pidref_legacy_fiber (connection, sender, &local_error);

          if (pidref)
            return dex_future_new_take_boxed (GLNX_TYPE_PID_REF,
                                              g_steal_pointer (&pidref));
        }
      return dex_future_new_for_error (g_steal_pointer (&local_error));
    }

  reply = dex_await_variant (dex_ref (dex_future_set_get_future_at (DEX_FUTURE_SET (future), 0)), NULL);
  fd_list = dex_await_object (dex_ref (dex_future_set_get_future_at (DEX_FUTURE_SET (future), 1)), NULL);

  g_variant_get (reply, "(@a{sv})", &dict);

  process_id = g_variant_lookup_value (dict, "ProcessID", G_VARIANT_TYPE_UINT32);
  if (!process_id)
    {
      pidref = connection_get_peer_pidref_legacy_fiber (connection, sender, &local_error);

      if (pidref)
        return dex_future_new_take_boxed (GLNX_TYPE_PID_REF,
                                          g_steal_pointer (&pidref));
      else
        return dex_future_new_for_error (g_steal_pointer (&local_error));
    }

  pid = g_variant_get_uint32 (process_id);

  process_fd = g_variant_lookup_value (dict, "ProcessFD", G_VARIANT_TYPE_HANDLE);
  if (!process_fd)
    {
      pidref = glnx_pid_ref_new_from_pid (pid, &local_error);
      if (!pidref)
        return dex_future_new_for_error (g_steal_pointer (&local_error));

      return dex_future_new_take_boxed (GLNX_TYPE_PID_REF,
                                        g_steal_pointer (&pidref));
    }

  fd_id = g_variant_get_handle (process_fd);

  if (fd_list == NULL)
    return dex_future_new_reject (G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer pidfd");

  if (!xdp_is_fd_list_index_valid (fd_list, fd_id))
    return dex_future_new_reject (G_IO_ERROR, G_IO_ERROR_FAILED, "Pidfd index is out of bounds");

  pidfd = g_unix_fd_list_get (fd_list, fd_id, &local_error);
  if (pidfd < 0)
    return dex_future_new_for_error (g_steal_pointer (&local_error));

  pidref = glnx_pid_ref_new_from_pidfd_take (g_steal_fd (&pidfd), &local_error);
  if (!pidref)
    return dex_future_new_for_error (g_steal_pointer (&local_error));

  return dex_future_new_take_boxed (GLNX_TYPE_PID_REF,
                                    g_steal_pointer (&pidref));
}

DexFuture *
xdp_connection_get_peer_pidref (GDBusConnection *connection,
                                const char      *sender)
{
  dex_return_error_if_fail (G_IS_DBUS_CONNECTION (connection));
  dex_return_error_if_fail (sender != NULL);

  return dex_scheduler_spawnv (NULL, 0,
                               G_CALLBACK (connection_get_peer_pidref_fiber),
                               2,
                               G_TYPE_DBUS_CONNECTION, connection,
                               G_TYPE_STRING, sender);
}

XdpAppInfo *
xdp_invocation_get_app_info (GDBusMethodInvocation *invocation)
{
  return g_object_get_data (G_OBJECT (invocation), "xdp-app-info");
}

gboolean
xdp_filter_options (GVariant            *options,
                    GVariantBuilder     *filtered,
                    const XdpOptionKey  *supported_options,
                    int                  n_supported_options,
                    gpointer             user_data,
                    GError             **error)
{
  int i;
  gboolean ret = TRUE;

  for (i = 0; i < n_supported_options; i++)
    {
      g_autoptr(GVariant) value = NULL;

      value = g_variant_lookup_value (options,
                                      supported_options[i].key,
                                      supported_options[i].type);
      if (!value)
        {
          value = g_variant_lookup_value (options, supported_options[i].key, NULL);
          if (value)
            {
              if (error && *error == NULL)
                g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                             "Expected type '%s' for option '%s', got '%s'",
                             g_variant_type_peek_string (supported_options[i].type),
                             supported_options[i].key,
                             g_variant_type_peek_string (g_variant_get_type (value)));
              ret = FALSE;
            }

          continue;
        }

      if (supported_options[i].validate)
        {
          g_autoptr(GError) local_error = NULL;

          if (!supported_options[i].validate (supported_options[i].key,
                                              value,
                                              options,
                                              user_data,
                                              &local_error))
            {
              if (error && *error == NULL)
                g_propagate_error (error, g_steal_pointer (&local_error));
              ret = FALSE;

              continue;
            }
        }

      g_variant_builder_add (filtered, "{sv}", supported_options[i].key, value);
    }

  return ret;
}

static const GDBusErrorEntry xdg_desktop_portal_error_entries[] = {
  { XDG_DESKTOP_PORTAL_ERROR_FAILED,           "org.freedesktop.portal.Error.Failed" },
  { XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT, "org.freedesktop.portal.Error.InvalidArgument" },
  { XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,        "org.freedesktop.portal.Error.NotFound" },
  { XDG_DESKTOP_PORTAL_ERROR_EXISTS,           "org.freedesktop.portal.Error.Exists" },
  { XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,      "org.freedesktop.portal.Error.NotAllowed" },
  { XDG_DESKTOP_PORTAL_ERROR_CANCELLED,        "org.freedesktop.portal.Error.Cancelled" },
  { XDG_DESKTOP_PORTAL_ERROR_WINDOW_DESTROYED, "org.freedesktop.portal.Error.WindowDestroyed" }
};

GQuark
xdg_desktop_portal_error_quark (void)
{
  static volatile gsize quark_volatile = 0;

  g_dbus_error_register_error_domain ("xdg-desktop-portal-error-quark",
                                      &quark_volatile,
                                      xdg_desktop_portal_error_entries,
                                      G_N_ELEMENTS (xdg_desktop_portal_error_entries));
  return (GQuark) quark_volatile;
}

static char *documents_mountpoint = NULL;

void
xdp_set_documents_mountpoint (const char *path)
{
  g_set_str (&documents_mountpoint, path);
}

const char *
xdp_get_documents_mountpoint (void)
{
  return documents_mountpoint;
}

/* alternate_document_path converts a file path  */
char *
xdp_get_alternate_document_path (const char *path,
                                 const char *app_id)
{
  int len;

  if (g_str_equal (app_id, ""))
    return NULL;

  /* If we don't know where the document portal is mounted, then there
   * is no alternate path */
  if (documents_mountpoint == NULL)
    return NULL;

  /* If the path is not within the document portal, then there is no
   * alternative path */
  len = strlen (documents_mountpoint);
  if (!g_str_has_prefix (path, documents_mountpoint) || path[len] != '/')
    return NULL;

  return g_strconcat (documents_mountpoint, "/by-app/", app_id, &path[len], NULL);
}

static gboolean
is_valid_name_character (gint c, gboolean allow_dash)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c >= '0' && c <= '9') ||
    (c == '_') || (allow_dash && c == '-');
}

/* This is the same as flatpak apps, except we also allow
   names to start with digits, and two-element names so that ids of the form
   snap.$snapname is allowed for all snap names. */
gboolean
xdp_is_valid_app_id (const char *string)
{
  guint len;
  const gchar *s;
  const gchar *end;
  const gchar *last_dot;
  int dot_count;
  gboolean last_element;

  g_return_val_if_fail (string != NULL, FALSE);

  len = strlen (string);
  if (G_UNLIKELY (len == 0))
    return FALSE;

  if (G_UNLIKELY (len > 255))
    return FALSE;

  end = string + len;

  last_dot = strrchr (string, '.');
  last_element = FALSE;

  s = string;
  if (G_UNLIKELY (*s == '.'))
    return FALSE; /* Name can't start with a period */

  dot_count = 0;
  while (s != end)
    {
      if (*s == '.')
        {
          if (s == last_dot)
            last_element = TRUE;
          s += 1;
          if (G_UNLIKELY (s == end))
            return FALSE;
          dot_count++;
        }

      if (G_UNLIKELY (!is_valid_name_character (*s, last_element)))
        return FALSE;
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 1))
    return FALSE;

  return TRUE;
}

gboolean
xdp_is_valid_token (const char *string)
{
  g_autofree char *path = NULL;
  int i;

  for (i = 0; string[i]; i++)
    {
      if (G_UNLIKELY (!is_valid_name_character (string[i], FALSE)))
        return FALSE;
    }

  path = g_strdup_printf ("/foo/%s", string);

  return g_variant_is_object_path (path);
}


char *
xdp_maybe_quote (const char *arg,
                 gboolean    quote_escape)
{
  if (quote_escape && needs_quoting (arg))
    return g_shell_quote (arg);
  else
    return g_strdup (arg);
}

char *
xdp_maybe_quote_argv (const char *argv[],
                      gboolean    quote_escape)
{
  GString *res = g_string_new ("");
  int i;

  for (i = 0; argv[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append_c (res, ' ');

      if (quote_escape && needs_quoting (argv[i]))
        {
          g_autofree char *quoted = g_shell_quote (argv[i]);
          g_string_append (res, quoted);
        }
      else
        g_string_append (res, argv[i]);
    }

  return g_string_free_and_steal (res);
}

typedef struct
{
  GError    *error;
  GError    *splice_error;
  GMainLoop *loop;
  int        refs;
} SpawnData;

static void
spawn_data_exit (SpawnData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
spawn_output_spliced_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  SpawnData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  spawn_data_exit (data);
}

static void
spawn_exit_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  SpawnData *data = user_data;

  g_subprocess_wait_check_finish (G_SUBPROCESS (obj), result, &data->error);
  spawn_data_exit (data);
}

char *
xdp_spawn (GError     **error,
           const char  *argv0,
           ...)
{
  g_autoptr(GPtrArray) args = NULL;
  const char *arg;
  va_list ap;
  char *output;

  va_start (ap, argv0);
  args = g_ptr_array_new ();
  g_ptr_array_add (args, (char *) argv0);
  while ((arg = va_arg (ap, const char *)))
    g_ptr_array_add (args, (char *) arg);
  g_ptr_array_add (args, NULL);
  va_end (ap);

  output = xdp_spawn_full ((const char * const *) args->pdata, -1, -1, error);

  return output;
}

char *
xdp_spawn_full (const char * const  *argv,
                int                  source_fd,
                int                  target_fd,
                GError             **error)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GInputStream *in;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GMainContext) context = NULL;
  SpawnData data = {0};
  g_autofree char *commandline = NULL;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);

  if (source_fd != -1)
    g_subprocess_launcher_take_fd (launcher, source_fd, target_fd);

  commandline = xdp_maybe_quote_argv ((const char **)argv, TRUE);
  g_debug ("Running: %s", commandline);

  subp = g_subprocess_launcher_spawnv (launcher, argv, error);

  if (subp == NULL)
    return NULL;

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  loop = g_main_loop_new (context, FALSE);

  data.loop = loop;
  data.refs = 2;

  in = g_subprocess_get_stdout_pipe (subp);
  out = g_memory_output_stream_new_resizable ();
  g_output_stream_splice_async (out,
                                in,
                                G_OUTPUT_STREAM_SPLICE_NONE,
                                0,
                                NULL,
                                spawn_output_spliced_cb,
                                &data);

  g_subprocess_wait_async (subp, NULL, spawn_exit_cb, &data);

  g_main_loop_run (loop);

  g_main_context_pop_thread_default (context);

  if (data.error)
    {
      g_propagate_error (error, g_steal_pointer (&data.error));
      g_clear_error (&data.splice_error);
      return NULL;
    }

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, g_steal_pointer (&data.splice_error));
          return NULL;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      g_output_stream_close (out, NULL, NULL);
      return g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return NULL;
}

gboolean
xdp_is_valid_filename (const char *filename)
{
  if (*filename == '\0')
    return FALSE;

  if (g_strcmp0 (filename, ".") == 0 || g_strcmp0 (filename, "..") == 0)
    return FALSE;

  if (strchr (filename, '/') != NULL)
    return FALSE;

  return TRUE;
}

char *
xdp_canonicalize_filename (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  return g_file_get_path (file);
}

gboolean
xdp_has_path_prefix (const char *str,
                     const char *prefix)
{
  while (TRUE)
    {
      /* Skip consecutive slashes to reach next path
         element */
      while (*str == '/')
        str++;
      while (*prefix == '/')
        prefix++;

      /* No more prefix path elements? Done! */
      if (*prefix == 0)
        return TRUE;

      /* Compare path element */
      while (*prefix != 0 && *prefix != '/')
        {
          if (*str != *prefix)
            return FALSE;
          str++;
          prefix++;
        }

      /* Matched prefix path element,
         must be entire str path element */
      if (*str != '/' && *str != 0)
        return FALSE;
    }
}

#define VALIDATOR_INPUT_FD 3
#define ICON_VALIDATOR_GROUP "Icon Validator"
#define SOUND_VALIDATOR_GROUP "Sound Validator"

static const char *
icon_type_to_string (XdpIconType icon_type)
{
  switch (icon_type)
    {
    case XDP_ICON_TYPE_DESKTOP:
      return "desktop";

    case XDP_ICON_TYPE_NOTIFICATION:
      return "notification";

    default:
      g_assert_not_reached ();
    }
}

gboolean
xdp_validate_icon (XdpSealedFd  *icon,
                   XdpIconType   icon_type,
                   char        **out_format,
                   char        **out_size)
{
  g_autofree char *format = NULL;
  g_autoptr(GError) error = NULL;
  const char *icon_validator = LIBEXECDIR "/xdg-desktop-portal-validate-icon";
  const char *args[7];
  int size;
  size_t i;
  g_autofree char *output = NULL;
  g_autoptr(GKeyFile) key_file = NULL;

  if (g_getenv ("XDP_VALIDATE_ICON"))
    icon_validator = g_getenv ("XDP_VALIDATE_ICON");

  if (!g_file_test (icon_validator, G_FILE_TEST_EXISTS))
    {
      g_warning ("Icon validation: %s not found, rejecting icon by default.", icon_validator);
      return FALSE;
    }

  i = 0;
  args[i++] = icon_validator;

  if (g_getenv ("XDP_VALIDATE_ICON_INSECURE") == NULL)
    args[i++] = "--sandbox";

  args[i++] = "--fd";
  args[i++] = G_STRINGIFY (VALIDATOR_INPUT_FD);
  args[i++] = "--ruleset";
  args[i++] = icon_type_to_string (icon_type);
  g_assert (i < G_N_ELEMENTS (args));
  args[i++] = NULL;

  output = xdp_spawn_full (args, xdp_sealed_fd_dup_fd (icon), VALIDATOR_INPUT_FD, &error);
  if (!output)
    {
      g_warning ("Icon validation: Rejecting icon because validator failed: %s", error->message);
      return FALSE;
    }

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_data (key_file, output, -1, G_KEY_FILE_NONE, &error))
    {
      g_warning ("Icon validation: %s", error->message);
      return FALSE;
    }
  if (!(format = g_key_file_get_string (key_file, ICON_VALIDATOR_GROUP, "format", &error)))
    {
      g_warning ("Icon validation: %s", error->message);
      return FALSE;
    }
  if (!(size = g_key_file_get_integer (key_file, ICON_VALIDATOR_GROUP, "width", &error)))
    {
      g_warning ("Icon validation: %s", error->message);
      return FALSE;
    }

  if (out_format)
    *out_format = g_steal_pointer (&format);
  if (out_size)
    *out_size = g_strdup_printf ("%d", size);

  return TRUE;
}

gboolean
xdp_validate_sound (XdpSealedFd *sound)
{
  const char *args[5];
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *output = NULL;
  const char *sound_validator = LIBEXECDIR "/xdg-desktop-portal-validate-sound";
  gsize i;

  if (g_getenv ("XDP_VALIDATE_SOUND"))
    sound_validator = g_getenv ("XDP_VALIDATE_SOUND");

  if (!g_file_test (sound_validator, G_FILE_TEST_EXISTS))
    {
      g_warning ("Sound validation: %s not found, rejecting sound by default.", sound_validator);
      return FALSE;
    }

  i = 0;
  args[i++] = sound_validator;

  if (g_getenv ("XDP_VALIDATE_SOUND_INSECURE") == NULL)
    args[i++] = "--sandbox";

  args[i++] = "--fd";
  args[i++] = G_STRINGIFY (VALIDATOR_INPUT_FD);
  g_assert (i < G_N_ELEMENTS (args));
  args[i++] = NULL;

  output = xdp_spawn_full (args, xdp_sealed_fd_dup_fd (sound), VALIDATOR_INPUT_FD, &error);
  if (!output)
    {
      g_warning ("Sound validation: Rejecting sound because validator failed: %s", error->message);
      return FALSE;
    }

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_data (key_file, output, -1, G_KEY_FILE_NONE, &error))
    {
      g_warning ("Sound validation: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

gboolean
xdp_variant_contains_key (GVariant *dictionary,
                          const char *key)
{
  GVariantIter iter;

  g_variant_iter_init (&iter, dictionary);
  while (TRUE)
    {
      g_autoptr(GVariant) entry = NULL;
      g_autoptr(GVariant) entry_key = NULL;

      entry = g_variant_iter_next_value (&iter);
      if (!entry)
        break;

      entry_key = g_variant_get_child_value (entry, 0);
      if (g_strcmp0 (g_variant_get_string (entry_key, NULL), key) == 0)
        return TRUE;
    }

  return FALSE;
}

int
xdp_get_portal_call_fd (GUnixFDList  *fd_list,
                        int           fd_id,
                        GError      **error)
{
  g_autofd int fd = -1;
  g_autoptr(GError) local_error = NULL;

  if (!xdp_is_fd_list_index_valid (fd_list, fd_id))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "File descriptor index %d is out of bounds (provided %d fds)",
                   fd_id, g_unix_fd_list_get_length (fd_list));
      return -1;
    }

  fd = g_unix_fd_list_get (fd_list, fd_id, &local_error);
  if (fd < 0)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   "Failed to get file descriptor: %s",
                   local_error->message);
    }

  return g_steal_fd (&fd);
}

int
xdp_peek_portal_call_fd (GUnixFDList  *fd_list,
                         int           fd_id,
                         GError      **error)
{
  const int *fds;

  if (!xdp_is_fd_list_index_valid (fd_list, fd_id))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "File descriptor index %d is out of bounds (provided %d fds)",
                   fd_id, g_unix_fd_list_get_length (fd_list));
      return -1;
    }

  fds = g_unix_fd_list_peek_fds (fd_list, NULL);
  return fds[fd_id];
}

gboolean
xdp_copy_fd_to_lists (GUnixFDList  *fd_list_src,
                      GUnixFDList  *fd_list_dst,
                      int           fd_id,
                      int          *fd_id_out,
                      GError      **error)
{
  g_autofd int fd = -1;
  int new_fd_id;

  if (!xdp_is_fd_list_index_valid (fd_list_src, fd_id))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "File descriptor index %d is out of bounds (provided %d fds)",
                   fd_id, g_unix_fd_list_get_length (fd_list_src));
      return FALSE;
    }

  fd = g_unix_fd_list_get (fd_list_src, fd_id, error);
  if (fd < 0)
    return FALSE;

  new_fd_id = g_unix_fd_list_append (fd_list_dst, fd, error);
  if (new_fd_id < 0)
    return FALSE;

  if (fd_id_out)
    *fd_id_out = new_fd_id;

  return TRUE;
}

/* Encode bytes as D-Bus-path-safe base64 ([A-Za-z0-9_], no padding).
 * We alias + and / to _ but that barely affects the information encoded. */
static char *
encode_base64_for_dbus (const uint8_t *data,
                        size_t         len)
{
  g_autofree char *encoded = g_base64_encode (data, len);

  for (size_t i = 0; encoded[i]; i++)
    {
      if (encoded[i] == '=')
        {
          encoded[i] = '\0';
          break;
        }

      if (encoded[i] == '+' || encoded[i] == '/')
        encoded[i] = '_';
    }

  return g_steal_pointer (&encoded);
}

char *
xdp_generate_token (void)
{
  uint32_t rng[4];

  for (size_t i = 0; i < G_N_ELEMENTS (rng); i++)
    rng[i] = g_random_int ();

  return encode_base64_for_dbus ((const uint8_t *) rng, sizeof (rng));
}

char *
xdp_generate_key (GError **error)
{
  uint8_t key[16];

  if (getrandom (key, sizeof (key), 0) != sizeof (key))
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to get random data: %s", g_strerror (errno));
      return NULL;
    }

  return encode_base64_for_dbus (key, sizeof (key));
}

char *
xdp_desktop_app_info_get_app_id (GDesktopAppInfo *info)
{
  const char *desktop_id;

  {
    g_autofree char *flatpak_id =
      g_desktop_app_info_get_string (info, "X-Flatpak");

    if (flatpak_id && *flatpak_id)
      return g_steal_pointer (&flatpak_id);
  }

  {
    g_autofree char *snap_instance =
      g_desktop_app_info_get_string (info, "X-SnapInstanceName");

    if (snap_instance && *snap_instance)
      return g_strconcat ("snap.", snap_instance, NULL);
  }

  desktop_id = g_app_info_get_id (G_APP_INFO (info));

  if (g_str_has_suffix (desktop_id, ".desktop"))
    return g_strndup (desktop_id, strlen (desktop_id) - strlen (".desktop"));

  return g_strdup (desktop_id);
}

gboolean
xdp_desktop_app_info_is_sandboxed (GDesktopAppInfo *info)
{
  {
    g_autofree char *flatpak_id =
      g_desktop_app_info_get_string (info, "X-Flatpak");

    if (flatpak_id && *flatpak_id)
      return TRUE;
  }

  {
    g_autofree char *snap_instance =
      g_desktop_app_info_get_string (info, "X-SnapInstanceName");

    if (snap_instance && *snap_instance)
      return TRUE;
  }

  return FALSE;
}

char *
xdp_desktop_app_info_get_doc_mountpoint (GDesktopAppInfo *info)
{
  {
    g_autofree char *flatpak_id =
      g_desktop_app_info_get_string (info, "X-Flatpak");

    if (flatpak_id && *flatpak_id)
      return g_build_filename ("/run/flatpak/doc", NULL);
  }

  return g_build_filename (g_get_user_runtime_dir (), "doc", NULL);
}
