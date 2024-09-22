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
                      g_propagate_error (error, g_steal_pointer (&local_error));
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

  commandline = xdp_maybe_quote_argv ((const char **)argv, TRUE);
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

static int
parse_pid (const char *str,
           pid_t      *pid)
{
  char *end;
  guint64 v;
  pid_t p;

  errno = 0;
  v = g_ascii_strtoull (str, &end, 0);
  if (end == str)
    return -ENOENT;
  else if (errno != 0)
    return -errno;

  p = (pid_t) v;

  if (p < 1 || (guint64) p != v)
    return -ERANGE;

  if (pid)
    *pid = p;

  return 0;
}

static int
parse_status_field_pid (const char *val,
                        pid_t      *pid)
{
  const char *t;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  return parse_pid (t, pid);
}

static pid_t
pidfd_to_pid (int         fdinfo,
              const int   pidfd,
              GError    **error)
{
  g_autofree char *name = NULL;
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  gboolean found = FALSE;
  FILE *f = NULL;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  int fd;
  int r = 0;
  pid_t pid = -1;

  name = g_strdup_printf ("%d", pidfd);

  fd = openat (fdinfo, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);

  if (fd != -1)
    f = fdopen (fd, "r");

  if (f == NULL)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Unable to open /proc/self/fdinfo/%d: %s",
                   fd, g_strerror (errno));
      return FALSE;
    }

  do {
    n = getdelim (&key, &keylen, ':', f);
    if (n == -1)
      {
        r = errno;
        break;
      }

    n = getdelim (&val, &vallen, '\n', f);
    if (n == -1)
      {
        r = errno;
        break;
      }

    g_strstrip (key);

    if (!strncmp (key, "Pid", 3))
      {
        r = parse_status_field_pid (val, &pid);
        found = r > -1;
      }

  } while (r == 0 && !found);

  fclose (f);

  if (r < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not parse fdinfo::%s: %s",
                   key, g_strerror (-r));
    }
  else if (!found)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not parse fdinfo: Pid field missing");
    }

  return pid;
}

static int
open_fdinfo_dir (GError **error)
{
  int fd;

  fd = open ("/proc/self/fdinfo", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);

  if (fd < 0)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Could not to open /proc/self/fdinfo: %s",
                 g_strerror (errno));

  return fd;
}

pid_t
xdp_pidfd_to_pid (int      pidfd,
                  GError **error)
{
  int fdinfo = -1;
  pid_t pid;

  g_return_val_if_fail (pidfd >= 0, -1);

  fdinfo = open_fdinfo_dir (error);
  if (fdinfo == -1)
    return -1;

  pid = pidfd_to_pid (fdinfo, pidfd, error);
  (void) close (fdinfo);

  return pid;
}

gboolean
xdp_pidfds_to_pids (const int  *pidfds,
                    pid_t      *pids,
                    gint        count,
                    GError    **error)
{
  int fdinfo = -1;
  int i;

  g_return_val_if_fail (pidfds != NULL, FALSE);
  g_return_val_if_fail (pids != NULL, FALSE);

  fdinfo = open_fdinfo_dir (error);
  if (fdinfo == -1)
    return FALSE;

  for (i = 0; i < count; i++)
    {
      pids[i] = pidfd_to_pid (fdinfo, pidfds[i], error);
      if (pids[i] < 0)
        break;
    }

  (void) close (fdinfo);

  return i == count;
}

gboolean
xdp_pidfd_get_namespace (int      pidfd,
                         ino_t   *ns,
                         GError **error)
{
  struct stat st;
  int r;

  g_return_val_if_fail (pidfd >= 0, FALSE);
  g_return_val_if_fail (ns != NULL, FALSE);

  r = fstatat (pidfd, "ns/pid", &st, 0);
  if (r == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not fstatat ns/pid: %s",
                   g_strerror (errno));
      return FALSE;
    }

  /* The inode number (together with the device ID) encode
   * the identity of the pid namespace, see namespaces(7)
   */
  *ns = st.st_ino;
  return TRUE;
}

static int
parse_status_field_uid (const char *val,
                        uid_t      *uid)
{
  const char *t;
  char *end;
  guint64 v;
  uid_t u;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  errno = 0;
  v = g_ascii_strtoull (t, &end, 0);
  if (end == val)
    return -ENOENT;
  else if (errno != 0)
    return -errno;

  u = (uid_t) v;

  if ((guint64) u != v)
    return -ERANGE;

  if (uid)
    *uid = u;

  return 0;
}

static int
parse_status_file (int    pid_fd,
                   pid_t *pid_out,
                   uid_t *uid_out)
{
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  gboolean have_pid = pid_out == NULL;
  gboolean have_uid = uid_out == NULL;
  FILE *f;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  int fd;
  int r = 0;

  g_return_val_if_fail (pid_fd > -1, FALSE);

  fd = openat (pid_fd, "status",  O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd == -1)
    return -errno;

  f = fdopen (fd, "r");

  if (f == NULL)
    return -errno;

  fd = -1; /* fd is now owned by f */

  do {
    n = getdelim (&key, &keylen, ':', f);
    if (n == -1)
      {
        r = -errno;
        break;
      }

    n = getdelim (&val, &vallen, '\n', f);
    if (n == -1)
      {
        r = -errno;
        break;
      }

    g_strstrip (key);
    g_strstrip (val);

    if (!strncmp (key, "NSpid", strlen ("NSpid")))
      {
        r = parse_status_field_pid (val, pid_out);
        have_pid = r > -1;
      }
    else if (!strncmp (key, "Uid", strlen ("Uid")))
      {
        r = parse_status_field_uid (val, uid_out);
        have_uid = r > -1;
      }

    if (r < 0)
      g_warning ("Failed to parse 'status::%s': %s",
                 key, g_strerror (-r));

  } while (r == 0 && (!have_uid || !have_pid));

  fclose (f);

  if (r != 0)
    return r;
  else if (!have_uid || !have_pid)
    return -ENXIO; /* ENOENT for the fields */

  return 0;
}

static inline gboolean
find_pid (pid_t *pids,
          guint  n_pids,
          pid_t  want,
          guint *idx)
{
  for (guint i = 0; i < n_pids; i++)
    {
      if (pids[i] == want)
        {
          *idx = i;
          return TRUE;
        }
    }

  return FALSE;
}

gboolean
xdp_map_pids_full (DIR     *proc,
                   ino_t    pidns,
                   pid_t   *pids,
                   guint    n_pids,
                   uid_t    target_uid,
                   GError **error)
{
  pid_t *res = NULL;
  struct dirent *de;
  guint count = 0;

  res = g_alloca (sizeof (pid_t) * n_pids);
  memset (res, 0, sizeof (pid_t) * n_pids);

  while ((de = readdir (proc)) != NULL)
    {
      xdp_autofd int pid_fd = -1;
      pid_t outside = 0;
      pid_t inside = 0;
      uid_t uid = 0;
      guint idx;
      ino_t ns = 0;
      int r;

      if (de->d_type != DT_DIR)
        continue;

      pid_fd = openat (dirfd (proc), de->d_name, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
      if (pid_fd == -1)
        continue;

      if (!xdp_pidfd_get_namespace (pid_fd, &ns, NULL))
        continue;

      if (pidns != ns)
        continue;

      r = parse_pid (de->d_name, &outside);
      if (r < 0)
        continue;

      r = parse_status_file (pid_fd, &inside, &uid);
      if (r < 0)
        continue;

      if (!find_pid (pids, n_pids, inside, &idx))
        continue;

      /* We got a match, let's make sure the real uids match as well */
      if (uid != target_uid)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                               "Matching pid doesn't belong to the target user");
          return FALSE;
        }

      /* this handles the first occurrence, already identified by find_pid,
       * as well as duplicate entries */
      for (guint i = idx; i < n_pids; i++)
        {
          if (pids[i] == inside)
            {
              res[idx] = outside;
              count++;
            }
        }
    }

  if (count != n_pids)
    {
      g_autoptr(GString) str = NULL;

      str = g_string_new ("Process ids could not be found: ");

      for (guint i = 0; i < n_pids; i++)
        if (res[i] == 0)
          g_string_append_printf (str, "%d, ", (guint32) pids[i]);

      g_string_truncate (str, str->len - 2);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, str->str);

      return FALSE;
    }

  memcpy (pids, res, sizeof (pid_t) * n_pids);

  return TRUE;
}

static gboolean
map_pids_proc (ino_t        pidns,
               pid_t       *pids,
               guint        n_pids,
               const char  *proc_dir,
               GError     **error)
{
  gboolean ok;
  DIR *proc;
  uid_t uid;

  g_return_val_if_fail (pidns > 0, FALSE);
  g_return_val_if_fail (pids != NULL, FALSE);

  proc = opendir (proc_dir);
  if (proc == NULL)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not open '%s': %s", proc_dir, g_strerror (errno));
      return FALSE;
    }

  uid = getuid ();

  ok = xdp_map_pids_full (proc, pidns, pids, n_pids, uid, error);
  closedir (proc);

  return ok;
}

gboolean
xdp_map_pids (ino_t    pidns,
              pid_t   *pids,
              guint    n_pids,
              GError **error)
{
  return map_pids_proc (pidns, pids, n_pids, "/proc", error);
}

gboolean
xdp_map_tids (ino_t    pidns,
              pid_t    owner_pid,
              pid_t   *tids,
              guint    n_tids,
              GError **error)
{
  g_autofree char *proc_dir = NULL;

  proc_dir = g_strdup_printf ("/proc/%u/task", (guint) owner_pid);

  return map_pids_proc (pidns, tids, n_tids, proc_dir, error);
}
