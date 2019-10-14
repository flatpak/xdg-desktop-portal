#include <config.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libportal/portal.h>

#include "src/xdp-dbus.h"

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Test"

char outdir[] = "/tmp/xdp-test-XXXXXX";

GTestDBus *dbus;
GDBusConnection *session_bus;
GSubprocess *portals;

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

static void
global_setup (void)
{
  GError *error = NULL;
  g_autofree gchar *services = NULL;
  g_autofree gchar *portal_dir = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  gboolean name_appeared = FALSE;
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

  portals = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_assert_no_error (error);

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);

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

  while (!name_appeared)
    g_main_context_iteration (NULL, TRUE);
}

static void
global_teardown (void)
{
  GError *error = NULL;
  g_autoptr(GFile) outdir_file = g_file_new_for_path (outdir);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_subprocess_force_exit (portals);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);
}

/* just check that the backend is there, and we have the expected
 * version of the portal
 */
static void
test_account_exists (void)
{
  g_autoptr(XdpAccountProxy) account = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *owner = NULL;

  account = XDP_ACCOUNT_PROXY (xdp_account_proxy_new_sync (session_bus,
                                                           0,
                                                           PORTAL_BUS_NAME,
                                                           PORTAL_OBJECT_PATH,
                                                           NULL,
                                                           &error));
  g_assert_no_error (error);

  owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (account));
  g_assert_nonnull (owner);

  g_assert_cmpuint (xdp_account_get_version (XDP_ACCOUNT (account)), ==, 1);
}

static gboolean got_info = FALSE;

static void
account_cb (GObject *obj,
            GAsyncResult *result,
            gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;
  GKeyFile *keyfile = data;
  gboolean res;
  const char *s;
  char *t;
  int response;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);

  ret = xdp_portal_get_user_information_finish (portal, result, &error);
  if (response == 0)
    {
      g_assert_no_error (error);
  
      t = g_key_file_get_string (keyfile, "account", "id", NULL);
      res = g_variant_lookup (ret, "id", "&s", &s); 
      g_assert (res == (t != NULL));
      if (t) g_assert_cmpstr (s, ==, t);

      t = g_key_file_get_string (keyfile, "account", "name", NULL);
      res = g_variant_lookup (ret, "name", "&s", &s); 
      g_assert (res == (t != NULL));
      if (t) g_assert_cmpstr (s, ==, t);

      t = g_key_file_get_string (keyfile, "account", "image", NULL);
      res = g_variant_lookup (ret, "image", "&s", &s); 
      g_assert (res == (t != NULL));
      if (t) g_assert_cmpstr (s, ==, t);
    }
  else if (response == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (response == 2)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  else
    g_assert_not_reached ();

  got_info = TRUE;

  g_main_context_wakeup (NULL);
}

static void
account_cb_fail (GObject *obj,
                 GAsyncResult *result,
                 gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;

  ret = xdp_portal_get_user_information_finish (portal, result, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);

  got_info = TRUE;
  g_main_context_wakeup (NULL);
}

/* some basic tests using libportal, and test that communication
 * with the backend via keyfile works
 */
static void
test_account_libportal (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "account", "id", "test");
  g_key_file_set_string (keyfile, "account", "name", "Donald Duck");
  g_key_file_set_string (keyfile, "account", "image", "");

  g_key_file_set_string (keyfile, "backend", "reason", "test");
  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_get_user_information (portal, NULL, "test", NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* check that the reason argument makes it to the backend
 */
static void
test_account_reason (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "account", "id", "test");
  g_key_file_set_string (keyfile, "account", "name", "Donald Duck");

  g_key_file_set_string (keyfile, "backend", "reason", "xx");
  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_get_user_information (portal, NULL, "xx", NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  got_info = FALSE;
  xdp_portal_get_user_information (portal, NULL, "yy", NULL, account_cb_fail, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

static void
test_account_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "account", "id", "test");
  g_key_file_set_string (keyfile, "account", "name", "Donald Duck");
  g_key_file_set_string (keyfile, "backend", "reason", "xx");
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_get_user_information (portal, NULL, "xx", NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

static void
test_account_user_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  /* simulate that the user cancels a hypothetical dialog,
   * by telling the backend to return 1 as response code.
   */
  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "account", "id", "test");
  g_key_file_set_string (keyfile, "account", "name", "Donald Duck");
  g_key_file_set_string (keyfile, "backend", "reason", "xx");
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_get_user_information (portal, NULL, "xx", NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

static gboolean
cancel_call (gpointer data)
{
  GCancellable *cancellable = data;

  g_debug ("cancel call");
  g_cancellable_cancel (cancellable);

  return G_SOURCE_REMOVE;
}

static void
test_account_app_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GCancellable) cancellable = NULL;

  /* cancel the interaction while the hypothetical dialog
   * is up.
   */
  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "account", "id", "test");
  g_key_file_set_string (keyfile, "account", "name", "Donald Duck");
  g_key_file_set_string (keyfile, "backend", "reason", "xx");
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = FALSE;
  xdp_portal_get_user_information (portal, NULL, "xx", cancellable, account_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  /* FIXME: we should verify that the backend got the Close()
   * and dismissed the dialog.
   */
}

int
main (int argc, char **argv)
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/portal/account/exists", test_account_exists);
  g_test_add_func ("/portal/account/libportal", test_account_libportal);
  g_test_add_func ("/portal/account/reason", test_account_reason);
  g_test_add_func ("/portal/account/delay", test_account_delay);
  g_test_add_func ("/portal/account/cancel/user", test_account_user_cancel);
  g_test_add_func ("/portal/account/cancel/app", test_account_app_cancel);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}

