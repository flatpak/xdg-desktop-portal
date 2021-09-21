/*
 * Copyright 2019-2021 Collabora Ltd.
 * Copyright 2021 Canonical Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "can-use-fuse.h"

#include <errno.h>
#include <unistd.h>

#include <glib/gstdio.h>

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>

gchar *cannot_use_fuse = NULL;

/*
 * If we cannot use FUSE, set cannot_use_fuse and return %FALSE.
 */
gboolean
check_fuse (void)
{
  g_autofree gchar *fusermount = NULL;
  g_autofree gchar *path = NULL;
  char *argv[] = { "flatpak-fuse-test" };
  const struct fuse_lowlevel_ops pc_oper = { 0 };
  struct fuse_args args = FUSE_ARGS_INIT (G_N_ELEMENTS (argv), argv);
  struct fuse_session *session = NULL;
  g_autoptr(GError) error = NULL;

  if (cannot_use_fuse != NULL)
    return FALSE;

  if (access ("/dev/fuse", W_OK) != 0)
    {
      cannot_use_fuse = g_strdup_printf ("access /dev/fuse: %s",
                                         g_strerror (errno));
      return FALSE;
    }

  fusermount = g_find_program_in_path ("fusermount3");

  if (fusermount == NULL)
    {
      cannot_use_fuse = g_strdup ("fusermount3 not found in PATH");
      return FALSE;
    }

  if (!g_file_test (fusermount, G_FILE_TEST_IS_EXECUTABLE))
    {
      cannot_use_fuse = g_strdup_printf ("%s not executable", fusermount);
      return FALSE;
    }

  if (!g_file_test ("/etc/mtab", G_FILE_TEST_EXISTS))
    {
      cannot_use_fuse = g_strdup ("fusermount3 won't work without /etc/mtab");
      return FALSE;
    }

  path = g_dir_make_tmp ("flatpak-test.XXXXXX", &error);
  g_assert_no_error (error);

  session = fuse_session_new (&args, &pc_oper, sizeof (pc_oper), &session);

  if (session == NULL)
    {
      fuse_opt_free_args (&args);
      cannot_use_fuse = g_strdup_printf ("fuse_mount: %s",
                                         g_strerror (errno));
      return FALSE;
    }

  if (fuse_session_mount (session, path) != 0)
    {
      fuse_opt_free_args (&args);
      fuse_session_destroy (session);
      cannot_use_fuse = g_strdup_printf ("fuse_mount: impossible to mount path "
                                         "'%s': %s",
                                         path, g_strerror (errno));
      return FALSE;
    }

  g_test_message ("Successfully set up test FUSE fs on %s", path);

  fuse_session_unmount (session);

  if (g_rmdir (path) != 0)
    g_error ("rmdir %s: %s", path, g_strerror (errno));

  fuse_opt_free_args (&args);
  fuse_session_destroy (session);

  return TRUE;
}

gboolean
check_fuse_or_skip_test (void)
{
  if (!check_fuse ())
    {
      g_assert (cannot_use_fuse != NULL);
      g_test_skip (cannot_use_fuse);
      return FALSE;
    }

  return TRUE;
}
