/*
 * Copyright © 2014 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <json-glib/json-glib.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixoutputstream.h>

#include "xdp-utils.h"

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

/* Based on g_mkstemp from glib */
gint
xdp_mkstempat (int    dir_fd,
               gchar *tmpl,
               int    flags,
               int    mode)
{
  char *XXXXXX;
  int count, fd;
  static const char letters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static const int NLETTERS = sizeof (letters) - 1;
  gint64 value;
  gint64 current_time;
  static int counter = 0;

  g_return_val_if_fail (tmpl != NULL, -1);

  /* find the last occurrence of "XXXXXX" */
  XXXXXX = g_strrstr (tmpl, "XXXXXX");

  if (!XXXXXX || strncmp (XXXXXX, "XXXXXX", 6))
    {
      errno = EINVAL;
      return -1;
    }

  /* Get some more or less random data.  */
  current_time = g_get_real_time ();
  value = ((current_time % G_USEC_PER_SEC) ^ (current_time / G_USEC_PER_SEC)) + counter++;

  for (count = 0; count < 100; value += 7777, ++count)
    {
      gint64 v = value;

      /* Fill in the random bits.  */
      XXXXXX[0] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[1] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[2] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[3] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[4] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[5] = letters[v % NLETTERS];

      fd = openat (dir_fd, tmpl, flags | O_CREAT | O_EXCL, mode);

      if (fd >= 0)
        return fd;
      else if (errno != EEXIST)
        /* Any other error will apply also to other names we might
         *  try, and there are 2^32 or so of them, so give up now.
         */
        return -1;
    }

  /* We got out of the loop because we ran out of combinations to try.  */
  errno = EEXIST;
  return -1;
}

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

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;
  XdpPeerDiedCallback peer_died_cb = user_data;

  if (!peer_died_cb)
    return;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] != ':' ||
      strcmp (name, from) != 0 ||
      strcmp (to, "") != 0)
    return;

  peer_died_cb (name);
}

void
xdp_connection_track_name_owners (GDBusConnection     *connection,
                                  XdpPeerDiedCallback  peer_died_cb)
{
  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      peer_died_cb, NULL);
}

gboolean
xdp_filter_options (GVariant *options,
                    GVariantBuilder *filtered,
                    XdpOptionKey *supported_options,
                    int n_supported_options,
                    GError **error)
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
              if (*error == NULL)
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

          if (!supported_options[i].validate (supported_options[i].key, value, options, &local_error))
            {
              if (ret)
                {
                  ret = FALSE;
                  if (error && *error == NULL)
                    {
                      g_propagate_error (error, local_error);
                      local_error = NULL;
                    }
                }

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
  g_clear_pointer (&documents_mountpoint, g_free);
  documents_mountpoint = g_strdup (path);
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

char *
xdp_get_app_id_from_desktop_id (const char *desktop_id)
{
  const gchar *suffix = ".desktop";
  if (g_str_has_suffix (desktop_id, suffix))
    return g_strndup (desktop_id, strlen (desktop_id) - strlen (suffix));
  else
    return g_strdup (desktop_id);
}

char *
xdp_quote_argv (const char *argv[])
{
  GString *res = g_string_new ("");
  int i;

  for (i = 0; argv[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append_c (res, ' ');

      if (needs_quoting (argv[i]))
        {
          g_autofree char *quoted = g_shell_quote (argv[i]);
          g_string_append (res, quoted);
        }
      else
        g_string_append (res, argv[i]);
    }

  return g_string_free (res, FALSE);
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
  GPtrArray *args;
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

  g_ptr_array_free (args, TRUE);

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
  SpawnData data = {0};
  g_autofree char *commandline = NULL;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);

  if (source_fd != -1)
    g_subprocess_launcher_take_fd (launcher, source_fd, target_fd);

  commandline = xdp_quote_argv ((const char **)argv);
  g_debug ("Running: %s", commandline);

  subp = g_subprocess_launcher_spawnv (launcher, argv, error);

  if (subp == NULL)
    return NULL;

  loop = g_main_loop_new (NULL, FALSE);

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

  if (data.error)
    {
      g_propagate_error (error, data.error);
      g_clear_error (&data.splice_error);
      return NULL;
    }

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, data.splice_error);
          return NULL;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      g_output_stream_close (out, NULL, NULL);
      return g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return NULL;
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
  g_autofree char *output = NULL;
  g_autoptr(GKeyFile) key_file = NULL;

  if (g_getenv ("XDP_VALIDATE_ICON"))
    icon_validator = g_getenv ("XDP_VALIDATE_ICON");

  if (!g_file_test (icon_validator, G_FILE_TEST_EXISTS))
    {
      g_warning ("Icon validation: %s not found, rejecting icon by default.", icon_validator);
      return FALSE;
    }

  args[0] = icon_validator;
  args[1] = "--sandbox";
  args[2] = "--fd";
  args[3] = G_STRINGIFY (VALIDATOR_INPUT_FD);
  args[4] = "--ruleset";
  args[5] = icon_type_to_string (icon_type);
  args[6] = NULL;

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
