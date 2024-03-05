/*
 * Copyright © 2018 Red Hat, Inc
 * Copyright © 2024 GNOME Foundation Inc.
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
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 *       Julian Sparber <jsparber@gnome.org>
 */

/* This is based on src/validate-icon.c */

#include <errno.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <glib/gstdio.h>
#include <unistd.h>

#ifdef __FreeBSD__
#define execvpe exect
#endif

#define SOUND_VALIDATOR_GROUP "Sound Validator"

static int
validate_sound (int input_fd)
{
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *key_file_data = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GstDiscoverer) discoverer = NULL;
  g_autoptr(GstDiscovererInfo) info = NULL;
  g_autoptr(GstDiscovererStreamInfo) audio_info = NULL;
  g_autoptr(GstDiscovererStreamInfo) stream_info = NULL;
  g_autoptr(GstDiscovererStreamInfo) stream_next = NULL;
  GstDiscovererResult result;
  const gchar *format = NULL;
  g_autofree gchar *uri = NULL;

  gst_init (NULL, NULL);

  discoverer = gst_discoverer_new (GST_SECOND, &error);

  if (!discoverer)
    {
      g_printerr ("validate-sound: Failed to create gstreamer discoverer: %s\n", error->message);
      return 1;
    }

  uri = g_strdup_printf ("file:///proc/self/fd/%d", input_fd);
  info = gst_discoverer_discover_uri (discoverer, uri, &error);
  result = gst_discoverer_info_get_result (info);

  switch (result)
    {
      case GST_DISCOVERER_URI_INVALID:
        g_assert_not_reached ();
        return 1;
      case GST_DISCOVERER_ERROR:
        g_printerr ("validate-sound: Couldn't discover media type: %s\n", error->message);
        return 1;
      case GST_DISCOVERER_TIMEOUT:
        g_printerr ("validate-sound: Couldn't discover media type: Timeout\n");
        return 1;
      case GST_DISCOVERER_BUSY:
        g_printerr ("validate-sound: Couldn't discover media type: Busy\n");
        return 1;
      case GST_DISCOVERER_MISSING_PLUGINS:
        {
          g_autofree char *str = NULL;

          str = g_strjoinv ("\n",
                            (char **) gst_discoverer_info_get_missing_elements_installer_details (info));

          g_printerr ("validate-sound: Couldn't discover media type: Missing plugins: %s\n", str);
          return 1;
        }
      case GST_DISCOVERER_OK:
        break;
    }

  stream_info = gst_discoverer_info_get_stream_info (info);
  if (!stream_info)
    {
      g_printerr ("validate-sound: Contains a invalid stream\n");
      return 1;
    }

  stream_next = gst_discoverer_stream_info_get_next (stream_info);
  if (stream_next)
    {
      g_printerr ("validate-sound: Only a single stream is allowed\n");
      return 1;
    }

  if (GST_IS_DISCOVERER_CONTAINER_INFO (stream_info))
    {
      g_autoptr(GList) streams = NULL;
      g_autoptr(GstCaps) container_caps = NULL;
      GstStructure *structure = NULL;

      container_caps = gst_discoverer_stream_info_get_caps (stream_info);
      structure = gst_caps_get_structure (container_caps, 0);

      if (!gst_caps_is_fixed (container_caps) || gst_caps_get_size (container_caps) != 1)
        {
          g_printerr ("validate-sound: The media format is to complex\n");
          return 1;
        }

      if (!gst_structure_has_name (structure, "audio/ogg"))
        {
          g_printerr ("validate-sound: Unsupported container format\n");
          return 1;
        }

      streams = gst_discoverer_container_info_get_streams (GST_DISCOVERER_CONTAINER_INFO (stream_info));

      if (streams->next)
        {
          g_printerr ("validate-sound: Only a single stream is allowed\n");
          return 1;
        }

      audio_info = gst_discoverer_stream_info_ref (streams->data);
    }
  else
    {
      audio_info = g_steal_pointer (&stream_info);
    }

  if (GST_IS_DISCOVERER_AUDIO_INFO (audio_info))
    {
      g_autoptr(GstCaps) caps = NULL;
      GstStructure *structure = NULL;

      caps = gst_discoverer_stream_info_get_caps (audio_info);
      structure = gst_caps_get_structure (caps, 0);

      if (!gst_caps_is_fixed (caps) || gst_caps_get_size (caps) != 1)
        {
          g_printerr ("validate-sound: Media format is to complex\n");
          return 1;
        }

      if (gst_structure_has_name (structure, "audio/x-wav"))
        {
          format = "wav/pcm";
        }
      else if (gst_structure_has_name (gst_caps_get_structure (caps, 0), "audio/x-vorbis"))
        {
          format = "ogg/vorbis";
        }
      else if (gst_structure_has_name (gst_caps_get_structure (caps, 0), "audio/x-opus"))
        {
          format = "ogg/opus";
        }
    }

  if (format == NULL)
    {
      g_printerr ("validate-sound: Unsupported sound format\n");
      return 1;
    }

  key_file = g_key_file_new ();
  g_key_file_set_string (key_file, SOUND_VALIDATOR_GROUP, "format", format);
  key_file_data = g_key_file_to_data (key_file, NULL, NULL);
  g_print ("%s", key_file_data);

  close (input_fd);

  return 0;
}

#ifdef HELPER

G_GNUC_NULL_TERMINATED
static void
add_args (GPtrArray *argv_array, ...)
{
  va_list args;
  const char *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, const gchar *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

static gboolean
path_is_usrmerged (const char *dir)
{
  /* does /dir point to /usr/dir? */
  g_autofree char *target = NULL;
  GStatBuf stat_buf_src, stat_buf_target;

  if (g_stat (dir, &stat_buf_src) < 0)
    return FALSE;

  target = g_strdup_printf ("/usr/%s", dir);

  if (g_stat (target, &stat_buf_target) < 0)
    return FALSE;

  return (stat_buf_src.st_dev == stat_buf_target.st_dev) &&
         (stat_buf_src.st_ino == stat_buf_target.st_ino);
}

const char *
flatpak_get_bwrap (void)
{
  const char *e = g_getenv ("FLATPAK_BWRAP");

  if (e != NULL)
    return e;

  return HELPER;
}

static int
rerun_in_sandbox (int input_fd)
{
  const char * const usrmerged_dirs[] = { "bin", "lib32", "lib64", "lib", "sbin" };
  int i;
  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  char validate_sound[PATH_MAX + 1];
  ssize_t symlink_size;
  g_autofree char* arg_input_fd = NULL;

  symlink_size = readlink ("/proc/self/exe", validate_sound, sizeof (validate_sound) - 1);
  if (symlink_size < 0 || (size_t) symlink_size >= sizeof (validate_sound))
    {
      g_printerr ("Error: failed to read /proc/self/exe\n");
      return 1;
    }

  validate_sound[symlink_size] = 0;

  add_args (args,
            flatpak_get_bwrap (),
            "--unshare-ipc",
            "--unshare-net",
            "--unshare-pid",
            "--ro-bind", "/usr", "/usr",
            "--ro-bind-try", "/etc/ld.so.cache", "/etc/ld.so.cache",
            "--ro-bind", validate_sound, validate_sound,
            NULL);

  /* These directories might be symlinks into /usr/... */
  for (i = 0; i < G_N_ELEMENTS (usrmerged_dirs); i++)
    {
      g_autofree char *absolute_dir = g_strdup_printf ("/%s", usrmerged_dirs[i]);

      if (!g_file_test (absolute_dir, G_FILE_TEST_EXISTS))
        continue;

      if (path_is_usrmerged (absolute_dir))
        {
          g_autofree char *symlink_target = g_strdup_printf ("/usr/%s", absolute_dir);

          add_args (args,
                    "--symlink", symlink_target, absolute_dir,
                    NULL);
        }
      else
        {
          add_args (args,
                    "--ro-bind", absolute_dir, absolute_dir,
                    NULL);
        }
    }

  add_args (args,
            "--tmpfs", "/tmp",
            "--proc", "/proc",
            "--dev", "/dev",
            "--chdir", "/",
            "--setenv", "GIO_USE_VFS", "local",
            "--unsetenv", "TMPDIR",
            "--die-with-parent",
            NULL);

  if (g_getenv ("G_MESSAGES_DEBUG"))
    add_args (args, "--setenv", "G_MESSAGES_DEBUG", g_getenv ("G_MESSAGES_DEBUG"), NULL);
  if (g_getenv ("G_MESSAGES_PREFIXED"))
    add_args (args, "--setenv", "G_MESSAGES_PREFIXED", g_getenv ("G_MESSAGES_PREFIXED"), NULL);


  arg_input_fd = g_strdup_printf ("%d", input_fd);
  add_args (args, validate_sound, "--fd", arg_input_fd, NULL);
  g_ptr_array_add (args, NULL);

  execvpe (flatpak_get_bwrap (), (char **) args->pdata, NULL);
  /* If we get here, then execvpe() failed. */
  g_printerr ("Sound validation: execvpe %s: %s\n", flatpak_get_bwrap (), g_strerror (errno));
  return 1;
}
#endif

static gboolean  opt_sandbox;
static gchar    *opt_path = NULL;
static gint      opt_fd = -1;

static GOptionEntry entries[] = {
  { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox, "Run in a sandbox", NULL },
  { "path", 0, 0, G_OPTION_ARG_FILENAME, &opt_path, "Read sound data from given file path", "PATH" },
  { "fd", 0, 0, G_OPTION_ARG_INT, &opt_fd, "Read sound data from given file descriptor", "FD" },
  { NULL }
};

int
main (int argc, char *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Error: %s\n", error->message);
      return 1;
    }

  if (opt_path != NULL && opt_fd != -1)
    {
      g_printerr ("Error: Only --path or --fd can be given\n");
      return 1;
    }

  if (opt_path)
    {
      opt_fd = g_open (opt_path, O_RDONLY, 0);
      if (opt_fd == -1)
        {
          g_printerr ("Error: Couldn't open file\n");
          return 1;
        }
    }
  else if (opt_fd == -1)
    {
      g_autofree char *help = NULL;

      help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("Error: Either --path or --fd needs to be given\n\n%s", help);
      return 1;
    }

#ifdef HELPER
  if (opt_sandbox)
    return rerun_in_sandbox (opt_fd);
  else
#endif
    return validate_sound (opt_fd);
}
