#include <config.h>

#include <gio/gio.h>

#include "src/xdp-dbus.h"

#ifdef HAVE_LIBPORTAL
#include "account.h"
#include "email.h"
#include "filechooser.h"
#include "print.h"
#include "screenshot.h"
#include "trash.h"
#endif

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Test"

char outdir[] = "/tmp/xdp-test-XXXXXX";

static GTestDBus *dbus;
static GDBusConnection *session_bus;
static GSubprocess *portals;
static GSubprocess *backends;

static void
name_appeared_cb (GDBusConnection *bus,
                  const char *name,
                  const char *name_owner,
                  gpointer data)
{
  gboolean *b = data;

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
global_setup (void)
{
  GError *error = NULL;
  g_autofree gchar *services = NULL;
  g_autofree gchar *portal_dir = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  gboolean name_appeared = FALSE;
  guint name_timeout;
  const char *argv[3];

  g_mkdtemp (outdir);
  g_print ("outdir: %s\n", outdir);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  services = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (dbus, services);
  g_test_dbus_up (dbus);

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  /* start portal backends */
  g_bus_watch_name_on_connection (session_bus,
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
 
  argv[0] = "./test-backends";
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  backends = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  name_timeout = g_timeout_add (1000, timeout_cb, "Failed to launch test-backends");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);

  name_appeared = FALSE;
  
  /* start portals */
  g_bus_watch_name_on_connection (session_bus,
                                  PORTAL_BUS_NAME,
                                  0,
                                  name_appeared_cb,
                                  name_disappeared_cb,
                                  &name_appeared,
                                  NULL);

  portal_dir = g_test_build_filename (G_TEST_DIST, "portals", NULL);

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "G_DEBUG", "fatal-criticals", TRUE);
  g_subprocess_launcher_setenv (launcher, "DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DESKTOP_PORTAL_DIR", portal_dir, TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", outdir, TRUE);
 
  argv[0] = "./xdg-desktop-portal";
  argv[1] = g_test_verbose () ? "--verbose" : NULL;
  argv[2] = NULL;

  portals = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  name_timeout = g_timeout_add (1000, timeout_cb, "Failed to launch xdg-desktop-portal");

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

  g_source_remove (name_timeout);
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
DEFINE_TEST_EXISTS(email, EMAIL, 2)
DEFINE_TEST_EXISTS(file_chooser, FILE_CHOOSER, 1)
DEFINE_TEST_EXISTS(game_mode, GAME_MODE, 3)
DEFINE_TEST_EXISTS(network_monitor, NETWORK_MONITOR, 3)
DEFINE_TEST_EXISTS(print, PRINT, 1)
DEFINE_TEST_EXISTS(proxy_resolver, PROXY_RESOLVER, 1)
DEFINE_TEST_EXISTS(screenshot, SCREENSHOT, 2)
DEFINE_TEST_EXISTS(settings, SETTINGS, 1)
DEFINE_TEST_EXISTS(trash, TRASH, 1)

int
main (int argc, char **argv)
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/portal/account/exists", test_account_exists);
  g_test_add_func ("/portal/email/exists", test_email_exists);
  g_test_add_func ("/portal/filechooser/exists", test_file_chooser_exists);
  g_test_add_func ("/portal/gamemode/exists", test_game_mode_exists);
  g_test_add_func ("/portal/networkmonitor/exists", test_network_monitor_exists);
  g_test_add_func ("/portal/print/exists", test_print_exists);
  g_test_add_func ("/portal/proxyresolver/exists", test_proxy_resolver_exists);
  g_test_add_func ("/portal/screenshot/exists", test_screenshot_exists);
  g_test_add_func ("/portal/settings/exists", test_settings_exists);
  g_test_add_func ("/portal/trash/exists", test_trash_exists);

#ifdef HAVE_LIBPORTAL
  g_test_add_func ("/portal/account/basic", test_account_libportal);
  g_test_add_func ("/portal/account/delay", test_account_delay);
  g_test_add_func ("/portal/account/cancel", test_account_cancel);
  g_test_add_func ("/portal/account/close", test_account_close);
  g_test_add_func ("/portal/account/reason", test_account_reason);

  g_test_add_func ("/portal/email/basic", test_email_libportal);
  g_test_add_func ("/portal/email/delay", test_email_delay);
  g_test_add_func ("/portal/email/cancel", test_email_cancel);
  g_test_add_func ("/portal/email/close", test_email_close);
  g_test_add_func ("/portal/email/address", test_email_address);
  g_test_add_func ("/portal/email/subject", test_email_subject);

  g_test_add_func ("/portal/screenshot/basic", test_screenshot_libportal);
  g_test_add_func ("/portal/screenshot/delay", test_screenshot_delay);
  g_test_add_func ("/portal/screenshot/cancel", test_screenshot_cancel);
  g_test_add_func ("/portal/screenshot/close", test_screenshot_close);

  g_test_add_func ("/portal/color/basic", test_color_libportal);
  g_test_add_func ("/portal/color/delay", test_color_delay);
  g_test_add_func ("/portal/color/cancel", test_color_cancel);
  g_test_add_func ("/portal/color/close", test_color_close);

  g_test_add_func ("/portal/trash/file", test_trash_file);

  g_test_add_func ("/portal/openfile/basic", test_open_file_libportal);
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

  g_test_add_func ("/portal/savefile/basic", test_save_file_libportal);
  g_test_add_func ("/portal/savefile/delay", test_save_file_delay);
  g_test_add_func ("/portal/savefile/close", test_save_file_close);
  g_test_add_func ("/portal/savefile/cancel", test_save_file_cancel);
  g_test_add_func ("/portal/savefile/filters", test_save_file_filters);
  g_test_add_func ("/portal/savefile/lockdown", test_save_file_lockdown);

  g_test_add_func ("/portal/prepareprint/basic", test_prepare_print_libportal);
  g_test_add_func ("/portal/prepareprint/delay", test_prepare_print_delay);
  g_test_add_func ("/portal/prepareprint/close", test_prepare_print_close);
  g_test_add_func ("/portal/prepareprint/cancel", test_prepare_print_cancel);
  g_test_add_func ("/portal/prepareprint/lockdown", test_prepare_print_lockdown);

  g_test_add_func ("/portal/print/basic", test_print_libportal);
  g_test_add_func ("/portal/print/delay", test_print_delay);
  g_test_add_func ("/portal/print/close", test_print_close);
  g_test_add_func ("/portal/print/cancel", test_print_cancel);
  g_test_add_func ("/portal/print/lockdown", test_print_lockdown);
#endif

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}

