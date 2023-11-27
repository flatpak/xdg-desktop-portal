#include "config.h"

#include <string.h>
#include <locale.h>

#include <gio/gio.h>

#include "src/glib-backports.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"
#include "xdp-impl-dbus.h"

#ifdef HAVE_LIBPORTAL
#include "account.h"
#include "background.h"
#include "camera.h"
#include "email.h"
#include "filechooser.h"
#include "inhibit.h"
#include "location.h"
#include "notification.h"
#include "openuri.h"
#include "print.h"
#include "screenshot.h"
#include "trash.h"
#include "wallpaper.h"
#endif

#include "utils.h"

/* required while we support meson + autotools. Autotools builds everything in
   the root dir ('.'), meson builds in each subdir nested and overrides these for
   g_test_build_filename */
#ifndef XDG_DP_BUILDDIR
#define XDG_DP_BUILDDIR "."
#endif
#ifndef XDG_PS_BUILDDIR
#define XDG_PS_BUILDDIR "."
#endif

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Limited"
#define BACKEND_OBJECT_PATH "/org/freedesktop/portal/desktop"

#include "document-portal/permission-store-dbus.h"

char outdir[] = "/tmp/xdp-test-XXXXXX";

static GTestDBus *dbus;
static GDBusConnection *session_bus;
static GList *test_procs = NULL;
XdpDbusImplPermissionStore *permission_store;
XdpDbusImplLockdown *lockdown;

int
xdup (int oldfd)
{
  int newfd = dup (oldfd);

  if (newfd < 0)
    g_error ("Unable to duplicate fd %d: %s", oldfd, g_strerror (errno));

  return newfd;
}

static void
name_appeared_cb (GDBusConnection *bus,
                  const char *name,
                  const char *name_owner,
                  gpointer data)
{
  gboolean *b = (gboolean *)data;

  g_debug ("Name %s now owned by %s\n", name, name_owner);

  *b = TRUE;

  g_main_context_wakeup (NULL);
}

static void
name_disappeared_cb (GDBusConnection *bus,
                     const char *name,
                     gpointer data)
{
  g_debug ("Name %s disappeared\n", name);
}

static gboolean
timeout_cb (gpointer data)
{
  const char *msg = data;

  g_error ("%s", msg);

  return G_SOURCE_REMOVE;
}

static void
update_data_dirs (void)
{
  const char *data_dirs;
  gssize len = 0;
  g_autoptr(GString) str = NULL;

  data_dirs = g_getenv ("XDG_DATA_DIRS");
  if (data_dirs != NULL &&
      strstr (data_dirs, "/usr/share") != NULL)
    {
      return;
    }

  if (data_dirs != NULL)
    {
      len = strlen (data_dirs);
      if (data_dirs[len] == ':')
        len--;
    }

  str = g_string_new_len (data_dirs, len);
  if (str->len > 0)
    g_string_append_c (str, ':');
  g_string_append (str, "/usr/local/share/:/usr/share/");

  g_debug ("Setting XDG_DATA_DIRS to %s", str->str);
  g_setenv ("XDG_DATA_DIRS", str->str, TRUE);
}

static void
global_setup (void)
{
  GError *error = NULL;
  g_autofree gchar *backends_executable = NULL;
  g_autofree gchar *services = NULL;
  g_autofree gchar *portal_dir = NULL;
  g_autofree gchar *argv0 = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  guint name_timeout;
  const char *argv[4];
  GQuark portal_errors G_GNUC_UNUSED;
  static gboolean name_appeared;
  guint watch;
  guint timeout_mult = 1;

  update_data_dirs ();

  g_mkdtemp (outdir);
  g_debug ("outdir: %s\n", outdir);

  g_setenv ("XDG_CURRENT_DESKTOP", "limited", TRUE);
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

  /* Re-defining dbus-daemon with a custom script */
  setup_dbus_daemon_wrapper (outdir);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  services = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (dbus, services);
  g_test_dbus_up (dbus);

  if (g_getenv ("TEST_IN_CI"))
    timeout_mult = 10;

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  /* start portal backends */
  name_appeared = FALSE;
  watch = g_bus_watch_name_on_connection (session_bus,
                                          BACKEND_BUS_NAME,
                                          0,
                                          name_appeared_cb,
                                          name_disappeared_cb,
                                          &name_appeared,
                                          NULL);

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
  g_subprocess_launcher_setenv (launcher, "PATH", g_getenv ("PATH"), TRUE);
  g_subprocess_launcher_take_stdout_fd (launcher, xdup (STDERR_FILENO));

  backends_executable = g_test_build_filename (G_TEST_BUILT, "test-backends", NULL);
  argv[0] = backends_executable;
  argv[1] = "--backend-name=" BACKEND_BUS_NAME;
  argv[2] = g_test_verbose () ? "--verbose" : NULL;
  argv[3] = NULL;

  g_debug ("launching test-backend\n");

  subprocess = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);
  g_test_message ("Launched %s with pid %s\n", argv[0],
                  g_subprocess_get_identifier (subprocess));
  test_procs = g_list_append (test_procs, g_steal_pointer (&subprocess));

  name_timeout = g_timeout_add (1000 * timeout_mult, timeout_cb, "Failed to launch test-backends");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);
  g_bus_unwatch_name (watch);

  /* start permission store */
  name_appeared = FALSE;
  watch = g_bus_watch_name_on_connection (session_bus,
                                          "org.freedesktop.impl.portal.PermissionStore",
                                          0,
                                          name_appeared_cb,
                                          name_disappeared_cb,
                                          &name_appeared,
                                          NULL);

  g_clear_object (&launcher);
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
  g_subprocess_launcher_setenv (launcher, "PATH", g_getenv ("PATH"), TRUE);
  g_subprocess_launcher_take_stdout_fd (launcher, xdup (STDERR_FILENO));

  if (g_getenv ("XDP_UNINSTALLED") != NULL)
    argv0 = g_test_build_filename (G_TEST_BUILT, "..", XDG_PS_BUILDDIR, "xdg-permission-store", NULL);
  else
    argv0 = g_strdup (LIBEXECDIR "/xdg-permission-store");

  argv[0] = argv0;
  argv[1] = "--replace";
  argv[2] = g_test_verbose () ? "--verbose" : NULL;
  argv[3] = NULL;

  g_debug ("launching %s\n", argv0);

  subprocess = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);
  g_test_message ("Launched %s with pid %s\n", argv[0],
                  g_subprocess_get_identifier (subprocess));
  test_procs = g_list_append (test_procs, g_steal_pointer (&subprocess));

  name_timeout = g_timeout_add (1000 * timeout_mult, timeout_cb, "Failed to launch xdg-permission-store");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);
  g_bus_unwatch_name (watch);

  /* start portals */
  name_appeared = FALSE;
  watch = g_bus_watch_name_on_connection (session_bus,
                                          PORTAL_BUS_NAME,
                                          0,
                                          name_appeared_cb,
                                          name_disappeared_cb,
                                          &name_appeared,
                                          NULL);

  portal_dir = g_test_build_filename (G_TEST_BUILT, "portals", "limited", NULL);

  g_clear_object (&launcher);
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DESKTOP_PORTAL_DIR", portal_dir, TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
  g_subprocess_launcher_setenv (launcher, "PATH", g_getenv ("PATH"), TRUE);
  g_subprocess_launcher_take_stdout_fd (launcher, xdup (STDERR_FILENO));

  g_clear_pointer (&argv0, g_free);

  if (g_getenv ("XDP_UNINSTALLED") != NULL)
    argv0 = g_test_build_filename (G_TEST_BUILT, "..", XDG_DP_BUILDDIR, "xdg-desktop-portal", NULL);
  else
    argv0 = g_strdup (LIBEXECDIR "/xdg-desktop-portal");

  argv[0] = argv0;
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  g_debug ("launching %s\n", argv0);

  subprocess = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);
  g_test_message ("Launched %s with pid %s\n", argv[0],
                  g_subprocess_get_identifier (subprocess));
  test_procs = g_list_append (test_procs, g_steal_pointer (&subprocess));
  g_clear_pointer (&argv0, g_free);

  name_timeout = g_timeout_add (1000 * timeout_mult, timeout_cb, "Failed to launch xdg-desktop-portal");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);
  g_bus_unwatch_name (watch);

  permission_store = xdp_dbus_impl_permission_store_proxy_new_sync (session_bus,
                                                                    0,
                                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                                    NULL,
                                                                    &error);
  g_assert_no_error (error);

  lockdown = xdp_dbus_impl_lockdown_proxy_new_sync (session_bus,
                                                    0,
                                                    BACKEND_BUS_NAME,
                                                    BACKEND_OBJECT_PATH,
                                                    NULL,
                                                    &error);
  g_assert_no_error (error);

  /* make sure errors are registered */
  portal_errors = XDG_DESKTOP_PORTAL_ERROR;
}

static void
wait_for_test_procs (void)
{
  GList *l;

  for (l = test_procs; l; l = l->next)
    {
      GSubprocess *subprocess = G_SUBPROCESS (l->data);
      GError *error = NULL;
      g_autofree char *identifier = NULL;

      identifier = g_strdup (g_subprocess_get_identifier (subprocess));

      g_debug ("Terminating and waiting for process %s", identifier);
      g_subprocess_send_signal (subprocess, SIGTERM);

      /* This may lead the test to hang, we assume that the test suite or CI
       * can handle the case at upper level, without having us async function
       * and timeouts */
      g_subprocess_wait (subprocess, NULL, &error);
      g_assert_no_error (error);
      g_assert_null (g_subprocess_get_identifier (subprocess));

      if (!g_subprocess_get_if_exited (subprocess))
        {
          g_assert_true (g_subprocess_get_if_signaled (subprocess));
          g_assert_cmpint (g_subprocess_get_term_sig (subprocess), ==, SIGTERM);
        }
      else if (!g_subprocess_get_successful (subprocess))
        {
          g_error ("Subprocess %s, exited with exit status %d", identifier,
                   g_subprocess_get_exit_status (subprocess));
        }
    }
}

static void
global_teardown (void)
{
  GError *error = NULL;

  g_dbus_connection_flush_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  wait_for_test_procs ();
  g_list_free_full (g_steal_pointer (&test_procs), g_object_unref);

  g_object_unref (lockdown);
  g_object_unref (permission_store);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);
}

/* Just check that the portal is there, and has the
 * expected version. This will fail if the backend
 * is not found.
 */
#define DEFINE_TEST_EXISTS(pp,PP,version) \
static void \
test_##pp##_exists (void) \
{ \
  g_autoptr(GDBusProxy) proxy = NULL; \
  g_autoptr(GError) error = NULL; \
  g_autofree char *owner = NULL; \
 \
  proxy = G_DBUS_PROXY (xdp_dbus_##pp##_proxy_new_sync (session_bus, \
                                                        0, \
                                                        PORTAL_BUS_NAME, \
                                                        PORTAL_OBJECT_PATH, \
                                                        NULL, \
                                                        &error)); \
  g_assert_no_error (error); \
 \
  owner = g_dbus_proxy_get_name_owner (proxy); \
  g_assert_nonnull (owner); \
 \
  g_assert_cmpuint (xdp_dbus_##pp##_get_version (XDP_DBUS_##PP (proxy)), ==, version); \
}

/* Just check that the portal is not there.
 *
 * We do a version check, but we hardcode the default value of zero,
 * as all portals will have a version greater than, or equal to one.
 */
#define DEFINE_TEST_DOES_NOT_EXIST(pp,PP) \
static void \
test_##pp##_does_not_exist (void) \
{ \
  g_autoptr(GDBusProxy) proxy = NULL; \
  g_autoptr(GError) error = NULL; \
  g_autofree char *owner = NULL; \
 \
  proxy = G_DBUS_PROXY (xdp_dbus_##pp##_proxy_new_sync (session_bus, \
                                                        0, \
                                                        PORTAL_BUS_NAME, \
                                                        PORTAL_OBJECT_PATH, \
                                                        NULL, \
                                                        &error)); \
  g_assert_no_error (error); \
 \
  owner = g_dbus_proxy_get_name_owner (proxy); \
  g_assert_nonnull (owner); \
 \
  g_assert_cmpuint (xdp_dbus_##pp##_get_version (XDP_DBUS_##PP (proxy)), ==, 0); \
}

DEFINE_TEST_EXISTS(file_chooser, FILE_CHOOSER, 4)

DEFINE_TEST_DOES_NOT_EXIST(print, PRINT)

int
main (int argc, char **argv)
{
  int res;

  /* Better leak reporting without gvfs */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_log_writer_default_set_use_stderr (TRUE);

  setlocale (LC_ALL, NULL);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/limited/filechooser/exists", test_file_chooser_exists);
  g_test_add_func ("/limited/print/does-not-exist", test_print_does_not_exist);

#ifdef HAVE_LIBPORTAL
  g_test_add_func ("/limited/openfile/basic", test_open_file_basic);
  g_test_add_func ("/limited/openfile/delay", test_open_file_delay);
  g_test_add_func ("/limited/openfile/close", test_open_file_close);
  g_test_add_func ("/limited/openfile/cancel", test_open_file_cancel);
  g_test_add_func ("/limited/openfile/multiple", test_open_file_multiple);
  g_test_add_func ("/limited/openfile/filters1", test_open_file_filters1);
  g_test_add_func ("/limited/openfile/filters2", test_open_file_filters2);
  g_test_add_func ("/limited/openfile/current_filter1", test_open_file_current_filter1);
  g_test_add_func ("/limited/openfile/current_filter2", test_open_file_current_filter2);
  g_test_add_func ("/limited/openfile/current_filter3", test_open_file_current_filter3);
  g_test_add_func ("/limited/openfile/current_filter4", test_open_file_current_filter4);
  g_test_add_func ("/limited/openfile/choices1", test_open_file_choices1);
  g_test_add_func ("/limited/openfile/choices2", test_open_file_choices2);
  g_test_add_func ("/limited/openfile/choices3", test_open_file_choices3);
  g_test_add_func ("/limited/openfile/parallel", test_open_file_parallel);

  g_test_add_func ("/limited/savefile/basic", test_save_file_basic);
  g_test_add_func ("/limited/savefile/delay", test_save_file_delay);
  g_test_add_func ("/limited/savefile/close", test_save_file_close);
  g_test_add_func ("/limited/savefile/cancel", test_save_file_cancel);
  g_test_add_func ("/limited/savefile/filters", test_save_file_filters);
  g_test_add_func ("/limited/savefile/lockdown", test_save_file_lockdown);
  g_test_add_func ("/limited/savefile/parallel", test_save_file_parallel);
#endif

  global_setup ();

  res = g_test_run ();

  sleep (1);

  global_teardown ();

  return res;
}
