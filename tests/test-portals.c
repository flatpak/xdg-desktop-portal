#include <config.h>
#include <string.h>
#include <locale.h>

#include <gio/gio.h>

#include "src/xdp-dbus.h"
#include "src/xdp-utils.h"
#include "src/xdp-impl-dbus.h"

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

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Test"
#define BACKEND_OBJECT_PATH "/org/freedesktop/portal/desktop"

#include "document-portal/permission-store-dbus.h"

char outdir[] = "/tmp/xdp-test-XXXXXX";

static GTestDBus *dbus;
static GDBusConnection *session_bus;
static GSubprocess *portals;
static GSubprocess *backends;
XdpImplPermissionStore *permission_store;
XdpImplLockdown *lockdown;

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
  GString *str;
  char *new_val;

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
  new_val = g_string_free (str, FALSE);

  g_debug ("Setting XDG_DATA_DIRS to %s", new_val);
  g_setenv ("XDG_DATA_DIRS", new_val, TRUE);
  /* new_val is leaked */
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
  guint name_timeout;
  const char *argv[4];
  GQuark portal_errors G_GNUC_UNUSED;
  static gboolean name_appeared;
  guint watch;
  guint timeout_mult = 1;

  update_data_dirs ();

  g_mkdtemp (outdir);
  g_print ("outdir: %s\n", outdir);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

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
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  g_print ("launching test-backend\n");

  backends = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  name_timeout = g_timeout_add (1000 * timeout_mult, timeout_cb, "Failed to launch test-backends");

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

  portal_dir = g_test_build_filename (G_TEST_DIST, "portals", NULL);

  g_clear_object (&launcher);
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DESKTOP_PORTAL_DIR", portal_dir, TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
  g_subprocess_launcher_setenv (launcher, "PATH", g_getenv ("PATH"), TRUE);
  g_subprocess_launcher_take_stdout_fd (launcher, xdup (STDERR_FILENO));

  if (g_getenv ("XDP_UNINSTALLED") != NULL)
    argv0 = g_test_build_filename (G_TEST_BUILT, "..", "xdg-desktop-portal", NULL);
  else
    argv0 = g_strdup (LIBEXECDIR "/xdg-desktop-portal");

  argv[0] = argv0;
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  g_print ("launching %s\n", argv0);

  portals = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);
  g_clear_pointer (&argv0, g_free);

  name_timeout = g_timeout_add (1000 * timeout_mult, timeout_cb, "Failed to launch xdg-desktop-portal");

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
    argv0 = g_test_build_filename (G_TEST_BUILT, "..", "xdg-permission-store", NULL);
  else
    argv0 = g_strdup (LIBEXECDIR "/xdg-permission-store");

  argv[0] = argv0;
  argv[1] = "--replace";
  argv[2] = g_test_verbose () ? "--verbose" : NULL;
  argv[3] = NULL;

  g_print ("launching %s\n", argv0);

  portals = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  name_timeout = g_timeout_add (1000 * timeout_mult, timeout_cb, "Failed to launch xdg-permission-store");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);
  g_bus_unwatch_name (watch);

  permission_store = xdp_impl_permission_store_proxy_new_sync (session_bus,
                                                               0,
                                                               "org.freedesktop.impl.portal.PermissionStore",
                                                               "/org/freedesktop/impl/portal/PermissionStore",
                                                               NULL,
                                                               &error);
  g_assert_no_error (error);

  lockdown = xdp_impl_lockdown_proxy_new_sync (session_bus,
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
global_teardown (void)
{
  GError *error = NULL;

  g_dbus_connection_flush_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_subprocess_force_exit (portals);
  g_subprocess_force_exit (backends);

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
#ifdef HAVE_PIPEWIRE
#define check_pipewire(name)
#else
#define check_pipewire(name) \
 if (strcmp (name , "camera") == 0) \
   { \
     g_test_skip ("Skipping tests that require pipewire"); \
     return; \
   }
#endif

#ifdef HAVE_GEOCLUE
#define check_geoclue(name)
#else
#define check_geoclue(name) \
 if (strcmp (name , "location") == 0) \
   { \
     g_test_skip ("Skipping tests that require geoclue"); \
     return; \
   }
#endif

#define DEFINE_TEST_EXISTS(pp,PP,version) \
static void \
test_##pp##_exists (void) \
{ \
  g_autoptr(GDBusProxy) proxy = NULL; \
  g_autoptr(GError) error = NULL; \
  g_autofree char *owner = NULL; \
 \
 check_pipewire ( #pp ) \
 check_geoclue ( #pp ) \
 \
  proxy = G_DBUS_PROXY (xdp_##pp##_proxy_new_sync (session_bus, \
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
  g_assert_cmpuint (xdp_##pp##_get_version (XDP_##PP (proxy)), ==, version); \
}

DEFINE_TEST_EXISTS(account, ACCOUNT, 1)
DEFINE_TEST_EXISTS(background, BACKGROUND, 1)
DEFINE_TEST_EXISTS(camera, CAMERA, 1)
DEFINE_TEST_EXISTS(email, EMAIL, 3)
DEFINE_TEST_EXISTS(file_chooser, FILE_CHOOSER, 3)
DEFINE_TEST_EXISTS(game_mode, GAME_MODE, 3)
DEFINE_TEST_EXISTS(inhibit, INHIBIT, 3)
DEFINE_TEST_EXISTS(location, LOCATION, 1)
DEFINE_TEST_EXISTS(network_monitor, NETWORK_MONITOR, 3)
DEFINE_TEST_EXISTS(notification, NOTIFICATION, 1)
DEFINE_TEST_EXISTS(open_uri, OPEN_URI, 3)
DEFINE_TEST_EXISTS(print, PRINT, 1)
DEFINE_TEST_EXISTS(proxy_resolver, PROXY_RESOLVER, 1)
DEFINE_TEST_EXISTS(screenshot, SCREENSHOT, 2)
DEFINE_TEST_EXISTS(settings, SETTINGS, 1)
DEFINE_TEST_EXISTS(trash, TRASH, 1)
DEFINE_TEST_EXISTS(wallpaper, WALLPAPER, 1)
DEFINE_TEST_EXISTS(realtime, REALTIME, 1)

int
main (int argc, char **argv)
{
  int res;

  setlocale (LC_ALL, NULL);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/portal/account/exists", test_account_exists);
  g_test_add_func ("/portal/background/exists", test_background_exists);
  g_test_add_func ("/portal/camera/exists", test_camera_exists);
  g_test_add_func ("/portal/email/exists", test_email_exists);
  g_test_add_func ("/portal/filechooser/exists", test_file_chooser_exists);
  g_test_add_func ("/portal/gamemode/exists", test_game_mode_exists);
  g_test_add_func ("/portal/inhibit/exists", test_inhibit_exists);
  g_test_add_func ("/portal/location/exists", test_location_exists);
  g_test_add_func ("/portal/networkmonitor/exists", test_network_monitor_exists);
  g_test_add_func ("/portal/notification/exists", test_notification_exists);
  g_test_add_func ("/portal/openuri/exists", test_open_uri_exists);
  g_test_add_func ("/portal/print/exists", test_print_exists);
  g_test_add_func ("/portal/proxyresolver/exists", test_proxy_resolver_exists);
  g_test_add_func ("/portal/screenshot/exists", test_screenshot_exists);
  g_test_add_func ("/portal/settings/exists", test_settings_exists);
  g_test_add_func ("/portal/trash/exists", test_trash_exists);
  g_test_add_func ("/portal/wallpaper/exists", test_wallpaper_exists);
  g_test_add_func ("/portal/realtime/exists", test_realtime_exists);

#ifdef HAVE_LIBPORTAL
  g_test_add_func ("/portal/account/basic", test_account_basic);
  g_test_add_func ("/portal/account/delay", test_account_delay);
  g_test_add_func ("/portal/account/cancel", test_account_cancel);
  g_test_add_func ("/portal/account/close", test_account_close);
  g_test_add_func ("/portal/account/reason", test_account_reason);
  g_test_add_func ("/portal/account/parallel", test_account_parallel);

  g_test_add_func ("/portal/email/basic", test_email_basic);
  g_test_add_func ("/portal/email/delay", test_email_delay);
  g_test_add_func ("/portal/email/cancel", test_email_cancel);
  g_test_add_func ("/portal/email/close", test_email_close);
  g_test_add_func ("/portal/email/address", test_email_address);
  g_test_add_func ("/portal/email/subject", test_email_subject);
  g_test_add_func ("/portal/email/parallel", test_email_parallel);

  g_test_add_func ("/portal/screenshot/basic", test_screenshot_basic);
  g_test_add_func ("/portal/screenshot/delay", test_screenshot_delay);
  g_test_add_func ("/portal/screenshot/cancel", test_screenshot_cancel);
  g_test_add_func ("/portal/screenshot/close", test_screenshot_close);
  g_test_add_func ("/portal/screenshot/parallel", test_screenshot_parallel);

  g_test_add_func ("/portal/color/basic", test_color_basic);
  g_test_add_func ("/portal/color/delay", test_color_delay);
  g_test_add_func ("/portal/color/cancel", test_color_cancel);
  g_test_add_func ("/portal/color/close", test_color_close);
  g_test_add_func ("/portal/color/parallel", test_color_parallel);

  g_test_add_func ("/portal/trash/file", test_trash_file);

  g_test_add_func ("/portal/openfile/basic", test_open_file_basic);
  g_test_add_func ("/portal/openfile/delay", test_open_file_delay);
  g_test_add_func ("/portal/openfile/close", test_open_file_close);
  g_test_add_func ("/portal/openfile/cancel", test_open_file_cancel);
  g_test_add_func ("/portal/openfile/multiple", test_open_file_multiple);
  g_test_add_func ("/portal/openfile/filters1", test_open_file_filters1);
  g_test_add_func ("/portal/openfile/filters2", test_open_file_filters2);
  g_test_add_func ("/portal/openfile/current_filter1", test_open_file_current_filter1);
  g_test_add_func ("/portal/openfile/current_filter2", test_open_file_current_filter2);
  g_test_add_func ("/portal/openfile/current_filter3", test_open_file_current_filter3);
  g_test_add_func ("/portal/openfile/current_filter4", test_open_file_current_filter4);
  g_test_add_func ("/portal/openfile/choices1", test_open_file_choices1);
  g_test_add_func ("/portal/openfile/choices2", test_open_file_choices2);
  g_test_add_func ("/portal/openfile/choices3", test_open_file_choices3);
  g_test_add_func ("/portal/openfile/parallel", test_open_file_parallel);

  g_test_add_func ("/portal/savefile/basic", test_save_file_basic);
  g_test_add_func ("/portal/savefile/delay", test_save_file_delay);
  g_test_add_func ("/portal/savefile/close", test_save_file_close);
  g_test_add_func ("/portal/savefile/cancel", test_save_file_cancel);
  g_test_add_func ("/portal/savefile/filters", test_save_file_filters);
  g_test_add_func ("/portal/savefile/lockdown", test_save_file_lockdown);
  g_test_add_func ("/portal/savefile/parallel", test_save_file_parallel);

  g_test_add_func ("/portal/prepareprint/basic", test_prepare_print_basic);
  g_test_add_func ("/portal/prepareprint/delay", test_prepare_print_delay);
  g_test_add_func ("/portal/prepareprint/close", test_prepare_print_close);
  g_test_add_func ("/portal/prepareprint/cancel", test_prepare_print_cancel);
  g_test_add_func ("/portal/prepareprint/lockdown", test_prepare_print_lockdown);
  g_test_add_func ("/portal/prepareprint/results", test_prepare_print_results);
  g_test_add_func ("/portal/prepareprint/parallel", test_prepare_print_parallel);

  g_test_add_func ("/portal/print/basic", test_print_basic);
  g_test_add_func ("/portal/print/delay", test_print_delay);
  g_test_add_func ("/portal/print/close", test_print_close);
  g_test_add_func ("/portal/print/cancel", test_print_cancel);
  g_test_add_func ("/portal/print/lockdown", test_print_lockdown);
  g_test_add_func ("/portal/print/parallel", test_print_parallel);

  g_test_add_func ("/portal/camera/basic", test_camera_basic);
  g_test_add_func ("/portal/camera/delay", test_camera_delay);
  g_test_add_func ("/portal/camera/close", test_camera_close);
  g_test_add_func ("/portal/camera/cancel", test_camera_cancel);
  g_test_add_func ("/portal/camera/lockdown", test_camera_lockdown);
  g_test_add_func ("/portal/camera/noaccess1", test_camera_no_access1);
  g_test_add_func ("/portal/camera/noaccess2", test_camera_no_access2);
  g_test_add_func ("/portal/camera/parallel", test_camera_parallel);

  g_test_add_func ("/portal/inhibit/basic", test_inhibit_basic);
  g_test_add_func ("/portal/inhibit/delay", test_inhibit_delay);
  g_test_add_func ("/portal/inhibit/close", test_inhibit_close);
  g_test_add_func ("/portal/inhibit/cancel", test_inhibit_cancel);
  g_test_add_func ("/portal/inhibit/parallel", test_inhibit_parallel);
  g_test_add_func ("/portal/inhibit/permissions", test_inhibit_permissions);
  g_test_add_func ("/portal/inhibit/monitor", test_inhibit_monitor);

  g_test_add_func ("/portal/openuri/http", test_open_uri_http);
  g_test_add_func ("/portal/openuri/http2", test_open_uri_http2);
  g_test_add_func ("/portal/openuri/file", test_open_uri_file);
  g_test_add_func ("/portal/openuri/delay", test_open_uri_delay);
  g_test_add_func ("/portal/openuri/close", test_open_uri_close);
  g_test_add_func ("/portal/openuri/cancel", test_open_uri_cancel);
  g_test_add_func ("/portal/openuri/lockdown", test_open_uri_lockdown);
  g_test_add_func ("/portal/openuri/directory", test_open_directory);

  g_test_add_func ("/portal/wallpaper/basic", test_wallpaper_basic);
  g_test_add_func ("/portal/wallpaper/delay", test_wallpaper_delay);
  g_test_add_func ("/portal/wallpaper/cancel1", test_wallpaper_cancel1);
  g_test_add_func ("/portal/wallpaper/cancel2", test_wallpaper_cancel2);
  g_test_add_func ("/portal/wallpaper/permission", test_wallpaper_permission);

  g_test_add_func ("/portal/location/basic", test_location_basic);
  g_test_add_func ("/portal/location/accuracy", test_location_accuracy);

  g_test_add_func ("/portal/background/basic1", test_background_basic1);
  g_test_add_func ("/portal/background/basic2", test_background_basic2);
  g_test_add_func ("/portal/background/commandline", test_background_commandline);
  g_test_add_func ("/portal/background/reason", test_background_reason);

  g_test_add_func ("/portal/notification/basic", test_notification_basic);
  g_test_add_func ("/portal/notification/buttons", test_notification_buttons);
  g_test_add_func ("/portal/notification/bad-arg", test_notification_bad_arg);
  g_test_add_func ("/portal/notification/bad-priority", test_notification_bad_priority);
  g_test_add_func ("/portal/notification/bad-button", test_notification_bad_button);
#endif

  global_setup ();

  res = g_test_run ();

  sleep (1);

  global_teardown ();

  return res;
}

