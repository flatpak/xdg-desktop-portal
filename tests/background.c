#include <config.h>

#include "background.h"

#include <libportal/portal.h>
#include "src/xdp-utils.h"

extern char outdir[];

static int got_info;

static void
background_cb (GObject *object,
               GAsyncResult *result,
               gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_request_background_finish (portal, result, &error);
  g_assert_true (res);
  g_assert_no_error (error);

  got_info = 1;
}

void
test_background_basic1 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_unref (keyfile);
  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "background", "reason", "Testing portals");
  g_key_file_set_boolean (keyfile, "background", "autostart", FALSE);
  g_key_file_set_boolean (keyfile, "background", "dbus_activatable", FALSE);

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);

  path = g_build_filename (outdir, "background", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "/bin/true");

  xdp_portal_request_background (portal, NULL, argv, "Testing portals", FALSE, FALSE, NULL, background_cb, NULL);

  while (got_info < 1)
    g_main_context_iteration (NULL, TRUE);
}

void
test_background_basic2 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_unref (keyfile);
  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "background", "reason", "Testing portals");
  g_key_file_set_boolean (keyfile, "background", "autostart", TRUE);
  g_key_file_set_boolean (keyfile, "background", "dbus_activatable", TRUE);

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);

  path = g_build_filename (outdir, "background", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "/bin/true");

  xdp_portal_request_background (portal, NULL, argv, "Testing portals", TRUE, TRUE, NULL, background_cb, NULL);

  while (got_info < 1)
    g_main_context_iteration (NULL, TRUE);
}

static void
background_fail (GObject *object,
                 GAsyncResult *result,
                 gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_request_background_finish (portal, result, &error);
  g_assert_false (res);
  g_assert_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT);

  got_info = 1;
}

void
test_background_commandline (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "background", "reason", "Testing portals");
  g_key_file_set_boolean (keyfile, "background", "autostart", TRUE);
  g_key_file_set_boolean (keyfile, "background", "dbus_activatable", TRUE);

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);

  path = g_build_filename (outdir, "background", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  argv = g_ptr_array_new ();

  xdp_portal_request_background (portal, NULL, argv, "Testing portals", TRUE, TRUE, NULL, background_fail, NULL);

  while (got_info < 1)
    g_main_context_iteration (NULL, TRUE);
}

void
test_background_reason (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "background", "reason", "Testing portals");
  g_key_file_set_boolean (keyfile, "background", "autostart", TRUE);
  g_key_file_set_boolean (keyfile, "background", "dbus_activatable", TRUE);

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);

  path = g_build_filename (outdir, "background", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  argv = g_ptr_array_new ();

  xdp_portal_request_background (portal, NULL, argv,
"012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
"012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
"012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789",
TRUE, TRUE, NULL, background_fail, NULL);

  while (got_info < 1)
    g_main_context_iteration (NULL, TRUE);
}
