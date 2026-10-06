/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "flatpak-instance.h"

typedef enum
{
  MONITOR_ALIVE,
  MONITOR_EXITED,
  MONITOR_ZERO,
  MONITOR_MISSING,
} MonitorState;

typedef struct
{
  char *dir;
  char *ref_path;
  FlatpakInstance *instance;
  pid_t holder;
  int release_fd;
} Fixture;

static char *runtime_dir;
static char *instances_dir;

static void
wait_for_child (pid_t pid)
{
  int status;
  pid_t result;

  do
    result = waitpid (pid, &status, 0);
  while (result == -1 && errno == EINTR);

  g_assert_cmpint (result, ==, pid);
  g_assert_true (WIFEXITED (status));
  g_assert_cmpint (WEXITSTATUS (status), ==, 0);
}

static void
write_file (Fixture    *fixture,
            const char *name,
            const char *contents)
{
  g_autofree char *path = g_build_filename (fixture->dir, name, NULL);
  g_autoptr(GError) error = NULL;

  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
}

static void
fixture_create (Fixture      *fixture,
                const char   *id,
                MonitorState  state)
{
  g_autoptr(GPtrArray) instances = NULL;
  g_autofree char *info_contents = NULL;
  g_autofree char *pid_contents = NULL;
  g_autofree char *bwrap_contents = NULL;
  pid_t pid = getpid ();

  fixture->dir = g_build_filename (instances_dir, id, NULL);
  fixture->ref_path = g_build_filename (fixture->dir, ".ref", NULL);
  fixture->release_fd = -1;
  g_assert_cmpint (g_mkdir (fixture->dir, 0700), ==, 0);

  if (state == MONITOR_EXITED)
    {
      pid = fork ();
      g_assert_cmpint (pid, >=, 0);
      if (pid == 0)
        _exit (0);
      wait_for_child (pid);
    }
  else if (state == MONITOR_ZERO)
    {
      pid = 0;
    }

  info_contents = g_strdup_printf ("[Application]\nname=org.example.Lifetime\n"
                                   "[Instance]\ninstance-id=%s\n", id);
  write_file (fixture, "info", info_contents);
  if (state != MONITOR_MISSING)
    {
      pid_contents = g_strdup_printf ("%ld\n", (long) pid);
      write_file (fixture, "pid", pid_contents);
    }
  bwrap_contents = g_strdup_printf ("{\"child-pid\": %ld}\n", (long) getpid ());
  write_file (fixture, "bwrapinfo.json", bwrap_contents);
  write_file (fixture, ".ref", "");

  instances = flatpak_instance_get_all ();
  for (guint i = 0; i < instances->len; i++)
    {
      FlatpakInstance *instance = g_ptr_array_index (instances, i);

      if (g_strcmp0 (flatpak_instance_get_id (instance), id) == 0)
        {
          fixture->instance = g_object_ref (instance);
          break;
        }
    }
  g_assert_nonnull (fixture->instance);
  g_assert_cmpstr (flatpak_instance_get_id (fixture->instance), ==, id);
  g_assert_cmpstr (flatpak_instance_get_app (fixture->instance), ==, "org.example.Lifetime");
  g_assert_cmpint (flatpak_instance_get_pid (fixture->instance), ==,
                   state == MONITOR_MISSING ? 0 : pid);
}

static void
fixture_setup (Fixture      *fixture,
               gconstpointer data)
{
  fixture_create (fixture, "123", GPOINTER_TO_INT (data));
}

/* F_GETLK reports locks owned by other processes. Use a real child to model
 * bubblewrap, and synchronize with pipes instead of relying on sleeps.
 */
static void
hold_reference (Fixture *fixture)
{
  int ready[2];
  int release[2];
  int child_error;
  ssize_t result;

  g_assert_cmpint (pipe (ready), ==, 0);
  g_assert_cmpint (pipe (release), ==, 0);
  fixture->holder = fork ();
  g_assert_cmpint (fixture->holder, >=, 0);

  if (fixture->holder == 0)
    {
      struct flock lock = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
      char byte;
      int fd;

      close (ready[0]);
      close (release[1]);
      fd = open (fixture->ref_path, O_RDONLY | O_CLOEXEC);
      child_error = fd == -1 ? errno : 0;
      if (child_error == 0 && fcntl (fd, F_SETLK, &lock) == -1)
        child_error = errno;

      do
        result = write (ready[1], &child_error, sizeof child_error);
      while (result == -1 && errno == EINTR);
      close (ready[1]);
      if (child_error != 0 || result != sizeof child_error)
        _exit (1);

      do
        result = read (release[0], &byte, 1);
      while (result == -1 && errno == EINTR);
      close (release[0]);
      close (fd);
      _exit (result >= 0 ? 0 : 1);
    }

  close (ready[1]);
  close (release[0]);
  fixture->release_fd = release[1];
  do
    result = read (ready[0], &child_error, sizeof child_error);
  while (result == -1 && errno == EINTR);
  close (ready[0]);
  g_assert_cmpint (result, ==, sizeof child_error);
  g_assert_cmpint (child_error, ==, 0);
}

static void
release_reference (Fixture *fixture)
{
  char byte = 0;
  ssize_t result;

  if (fixture->holder == 0)
    return;

  /* Signal explicitly: another holder forked later may have inherited a
   * copy of this pipe's write end. EOF still handles parent failure.
   */
  do
    result = write (fixture->release_fd, &byte, 1);
  while (result == -1 && errno == EINTR);
  g_assert_cmpint (result, ==, 1);
  close (fixture->release_fd);
  fixture->release_fd = -1;
  wait_for_child (fixture->holder);
  fixture->holder = 0;
}

static void
fixture_teardown (Fixture      *fixture,
                  gconstpointer data)
{
  const char *files[] = { "info", "pid", "bwrapinfo.json", ".ref", "ref-target" };

  release_reference (fixture);
  g_clear_object (&fixture->instance);
  for (size_t i = 0; i < G_N_ELEMENTS (files); i++)
    {
      g_autofree char *path = g_build_filename (fixture->dir, files[i], NULL);

      if (g_unlink (path) != 0)
        g_assert_cmpint (errno, ==, ENOENT);
    }
  g_assert_cmpint (g_rmdir (fixture->dir), ==, 0);
  g_free (fixture->dir);
  g_free (fixture->ref_path);
}

static void
test_held_reference (Fixture      *fixture,
                     gconstpointer data)
{
  hold_reference (fixture);
  g_assert_true (flatpak_instance_is_running (fixture->instance));
}

static void
test_unheld_reference (Fixture      *fixture,
                       gconstpointer data)
{
  g_assert_false (flatpak_instance_is_running (fixture->instance));
}

static void
test_reference_released (Fixture      *fixture,
                         gconstpointer data)
{
  hold_reference (fixture);
  g_assert_true (flatpak_instance_is_running (fixture->instance));
  release_reference (fixture);
  g_assert_false (flatpak_instance_is_running (fixture->instance));
}

static void
test_reference_missing (Fixture      *fixture,
                        gconstpointer data)
{
  g_assert_cmpint (g_unlink (fixture->ref_path), ==, 0);
  g_assert_false (flatpak_instance_is_running (fixture->instance));
}

static void
test_reference_symlink (Fixture      *fixture,
                        gconstpointer data)
{
  g_autofree char *target = g_build_filename (fixture->dir, "ref-target", NULL);

  hold_reference (fixture);
  g_assert_cmpint (g_rename (fixture->ref_path, target), ==, 0);
  g_assert_cmpint (symlink ("ref-target", fixture->ref_path), ==, 0);
  g_assert_false (flatpak_instance_is_running (fixture->instance));
}

static void
test_reference_fifo (Fixture      *fixture,
                     gconstpointer data)
{
  g_assert_cmpint (g_unlink (fixture->ref_path), ==, 0);
  g_assert_cmpint (mkfifo (fixture->ref_path, 0600), ==, 0);
  g_assert_false (flatpak_instance_is_running (fixture->instance));
}

static void
test_reference_unreadable (Fixture      *fixture,
                           gconstpointer data)
{
  int fd;

  g_assert_cmpint (g_chmod (fixture->ref_path, 0000), ==, 0);
  fd = open (fixture->ref_path, O_RDONLY | O_CLOEXEC);
  if (fd != -1)
    {
      close (fd);
      g_test_skip ("Process can read a reference with mode 0000");
    }
  else
    {
      g_assert_cmpint (errno, ==, EACCES);
      g_assert_false (flatpak_instance_is_running (fixture->instance));
    }
  g_assert_cmpint (g_chmod (fixture->ref_path, 0600), ==, 0);
}

static void
test_independent_instances (Fixture      *fixture,
                           gconstpointer data)
{
  Fixture other = { 0 };

  fixture_create (&other, "456", MONITOR_ALIVE);
  hold_reference (fixture);
  g_assert_true (flatpak_instance_is_running (fixture->instance));
  g_assert_false (flatpak_instance_is_running (other.instance));
  hold_reference (&other);
  release_reference (fixture);
  g_assert_false (flatpak_instance_is_running (fixture->instance));
  g_assert_true (flatpak_instance_is_running (other.instance));
  fixture_teardown (&other, NULL);
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  int result;

  /* Set this before GLib caches the user runtime directory. These tests
   * must never enumerate or modify the user's real Flatpak instances.
   */
  runtime_dir = g_dir_make_tmp ("xdp-flatpak-instance-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (runtime_dir);
  g_setenv ("XDG_RUNTIME_DIR", runtime_dir, TRUE);
  g_assert_cmpstr (g_get_user_runtime_dir (), ==, runtime_dir);
  instances_dir = g_build_filename (runtime_dir, ".flatpak", NULL);
  g_assert_cmpint (g_mkdir (instances_dir, 0700), ==, 0);

  g_test_init (&argc, &argv, NULL);
  g_test_add ("/flatpak-instance/held", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_held_reference, fixture_teardown);
  g_test_add ("/flatpak-instance/relaunch", Fixture, GINT_TO_POINTER (MONITOR_EXITED),
              fixture_setup, test_held_reference, fixture_teardown);
  g_test_add ("/flatpak-instance/startup", Fixture, GINT_TO_POINTER (MONITOR_MISSING),
              fixture_setup, test_held_reference, fixture_teardown);
  g_test_add ("/flatpak-instance/unheld", Fixture, GINT_TO_POINTER (MONITOR_EXITED),
              fixture_setup, test_unheld_reference, fixture_teardown);
  g_test_add ("/flatpak-instance/reused-pid", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_unheld_reference, fixture_teardown);
  g_test_add ("/flatpak-instance/zero-pid", Fixture, GINT_TO_POINTER (MONITOR_ZERO),
              fixture_setup, test_unheld_reference, fixture_teardown);
  g_test_add ("/flatpak-instance/released", Fixture, GINT_TO_POINTER (MONITOR_EXITED),
              fixture_setup, test_reference_released, fixture_teardown);
  g_test_add ("/flatpak-instance/missing-reference", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_reference_missing, fixture_teardown);
  g_test_add ("/flatpak-instance/symlink-reference", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_reference_symlink, fixture_teardown);
  g_test_add ("/flatpak-instance/fifo-reference", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_reference_fifo, fixture_teardown);
  g_test_add ("/flatpak-instance/unreadable-reference", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_reference_unreadable, fixture_teardown);
  g_test_add ("/flatpak-instance/independent-instances", Fixture, GINT_TO_POINTER (MONITOR_ALIVE),
              fixture_setup, test_independent_instances, fixture_teardown);

  result = g_test_run ();
  g_assert_cmpint (g_rmdir (instances_dir), ==, 0);
  g_assert_cmpint (g_rmdir (runtime_dir), ==, 0);
  g_free (instances_dir);
  g_free (runtime_dir);
  return result;
}
