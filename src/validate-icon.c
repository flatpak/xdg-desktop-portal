/*
 * Copyright © 2018 Red Hat, Inc
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

/* The canonical copy of this file is in:
 * - https://github.com/flatpak/flatpak at icon-validator/validate-icon.c
 * Known copies of this file are in:
 * - https://github.com/flatpak/xdg-desktop-portal at src/validate-icon.c
 */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "xdp-utils.h"

#ifdef __FreeBSD__
#define execvpe exect
#endif

#define ICON_VALIDATOR_GROUP "Icon Validator"

typedef struct
{
  const char *name;
  size_t max_icon_size;
  size_t max_svg_icon_size;
  size_t max_file_size;
} XdpValidatorRuleset;

static const XdpValidatorRuleset rulesets[] =
{
  {
    .name = "desktop",
    .max_icon_size = 512,
    .max_svg_icon_size = 4096,
    .max_file_size = 1024 * 1024 * 4 /* 4MB */,
  },
  {
    .name = "notification",
    .max_icon_size = 512,
    .max_svg_icon_size = 4096,
    .max_file_size = 1024 * 1024 * 4 /* 4MB */,
  },
};

static const XdpValidatorRuleset *ruleset = NULL;
static gboolean opt_sandbox;
static char *opt_path = NULL;
static int opt_fd = -1;

static gboolean
option_validator_cb (const gchar  *option_name,
                     const gchar  *value,
                     gpointer      data,
                     GError      **error)
{
  for (size_t i = 0; i < G_N_ELEMENTS (rulesets); i++)
    {
      if (g_strcmp0 (value, rulesets[i].name) == 0)
        {
          ruleset = &rulesets[i];
          return TRUE;
        }
    }

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
               "Invalid ruleset '%s'. Accepted values are: desktop, notification",
               value);
  return FALSE;
}

static GOptionEntry entries[] = {
  { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox, "Run in a sandbox", NULL },
  { "path", 0, 0, G_OPTION_ARG_FILENAME, &opt_path, "Read icon data from given file path. Required to be from a trusted source.", "PATH" },
  { "fd", 0, 0, G_OPTION_ARG_INT, &opt_fd, "Read icon data from given file descriptor. Required to be from a trusted source or to be sealed", "FD" },
  { "ruleset", 0, 0, G_OPTION_ARG_CALLBACK, &option_validator_cb, "The icon validator ruleset to apply. Accepted values: desktop, notification", "RULESET" },
  { NULL }
};

static int
validate_icon (int input_fd)
{
  const char *allowed_formats[] = { "png", "jpeg", "svg", NULL };
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  GdkPixbufFormat *format;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *key_file_data = NULL;
  g_autoptr(GdkPixbufLoader) loader = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  int max_size, width, height;
  g_autofree char *name = NULL;
  GdkPixbuf *pixbuf;

  g_assert (ruleset != NULL);

  /* Ensure that we read from the beginning of the file */
  lseek (input_fd, 0, SEEK_SET);

  mapped = g_mapped_file_new_from_fd (input_fd, FALSE, &error);
  if (!mapped)
    {
      g_printerr ("Failed to create mapped file for image: %s\n", error->message);
      return 1;
    }

  bytes = g_mapped_file_get_bytes (mapped);

  if (g_bytes_get_size (bytes) == 0)
    {
      g_printerr ("Image is 0 bytes\n");
      return 1;
    }

  if (g_bytes_get_size (bytes) > ruleset->max_file_size)
    {
      g_printerr ("Image is bigger then the allowed size\n");
      return 1;
    }

  loader = gdk_pixbuf_loader_new ();

  if (!gdk_pixbuf_loader_write_bytes (loader, bytes, &error))
    {
      g_printerr ("Failed to load image: %s\n", error->message);
      gdk_pixbuf_loader_close (loader, NULL);
      return 1;
    }

  if (!gdk_pixbuf_loader_close (loader, &error))
    {
      g_printerr ("Failed to load image: %s\n", error->message);
      return 1;
    }

  pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
  if (!pixbuf)
    {
      g_printerr ("Failed to load image: %s\n", error->message);
      return 1;
    }

  format = gdk_pixbuf_loader_get_format (loader);
  if (!format)
    {
      g_printerr ("Image format not recognized\n");
      return 1;
    }

  name = gdk_pixbuf_format_get_name (format);
  if (!g_strv_contains (allowed_formats, name))
    {
      g_printerr ("Image format %s not accepted\n", name);
      return 1;
    }

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);

  if (width != height)
    {
      g_printerr ("Expected a square image but got: %dx%d\n", width, height);
      return 1;
    }

  /* Sanity check for vector files */
  max_size = g_str_equal (name, "svg") ? ruleset->max_svg_icon_size : ruleset->max_icon_size;

  /* The icon is a square so we only need to check one side */
  if (width > max_size)
    {
      g_printerr ("Image too large (%dx%d). Max. size %dx%d\n", width, height, max_size, max_size);
      return 1;
    }

  /* Print the format and size for consumption by (at least) the dynamic
   * launcher portal. Use a GKeyFile so the output can be easily extended
   * in the future in a backwards compatible way.
   */
  key_file = g_key_file_new ();
  g_key_file_set_string (key_file, ICON_VALIDATOR_GROUP, "format", name);
  g_key_file_set_integer (key_file, ICON_VALIDATOR_GROUP, "width", width);
  key_file_data = g_key_file_to_data (key_file, NULL, NULL);
  g_print ("%s", key_file_data);

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
  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  g_autofree char* arg_input_fd = NULL;
  char validate_icon[PATH_MAX + 1];
  ssize_t symlink_size;

  g_assert (ruleset != NULL);

  symlink_size = readlink ("/proc/self/exe", validate_icon, sizeof (validate_icon) - 1);
  if (symlink_size < 0 || (size_t) symlink_size >= sizeof (validate_icon))
    {
      g_printerr ("Error: failed to read /proc/self/exe\n");
      return 1;
    }

  validate_icon[symlink_size] = 0;

  add_args (args,
            flatpak_get_bwrap (),
            "--unshare-ipc",
            "--unshare-net",
            "--unshare-pid",
            "--tmpfs", "/tmp",
            "--proc", "/proc",
            "--dev", "/dev",
            "--ro-bind", "/usr", "/usr",
            "--ro-bind-try", "/etc/ld.so.cache", "/etc/ld.so.cache",
            "--ro-bind", validate_icon, validate_icon,
            NULL);

  /* These directories might be symlinks into /usr/... */
  for (size_t i = 0; i < G_N_ELEMENTS (usrmerged_dirs); i++)
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
  add_args (args,
            validate_icon,
            "--fd", arg_input_fd,
            "--ruleset", ruleset->name,
            NULL);
  g_ptr_array_add (args, NULL);

  execvpe (flatpak_get_bwrap (), (char **) args->pdata, NULL);
  /* If we get here, then execvpe() failed. */
  g_printerr ("Icon validation: execvpe %s: %s\n", flatpak_get_bwrap (), g_strerror (errno));
  return 1;
}
#endif

int
main (int argc, char *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autofd int fd_path = -1;

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Error: %s\n", error->message);
      return 1;
    }

  if (ruleset == NULL)
    {
      g_printerr ("Error: A ruleset must be given with --ruleset\n");
      return 1;
    }

  if (opt_path != NULL && opt_fd != -1)
    {
      g_printerr ("Error: Only --path or --fd can be given\n");
      return 1;
    }

  if (opt_path)
    {
      opt_fd = fd_path = g_open (opt_path, O_RDONLY, 0);
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
    return validate_icon (opt_fd);
}
