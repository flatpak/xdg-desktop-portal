#include <config.h>

#include "screenshot.h"
#include "utils.h"

#include <libportal/portal.h>
#include "xdp-impl-dbus.h"

extern char outdir[];

static int got_info;

extern XdpDbusImplPermissionStore *permission_store;

static void
set_screenshot_permissions (const char *permission)
{
  const char *permissions[2] = { NULL, NULL };
  g_autoptr(GError) error = NULL;

  tests_set_app_id ("furrfix", &error);
  g_assert_no_error (error);

  permissions[0] = permission;
  xdp_dbus_impl_permission_store_call_set_permission_sync (permission_store,
                                                           "screenshot",
                                                           TRUE,
                                                           "screenshot",
                                                           tests_get_expected_app_id (),
                                                           permissions,
                                                           NULL,
                                                           &error);
  g_assert_no_error (error);
}

static void
reset_screenshot_permissions (void)
{
  GError *error = NULL;

  set_screenshot_permissions (NULL);
  tests_set_app_id (NULL, &error);
  g_assert_no_error (error);
}

static void
screenshot_cb (GObject *obj,
               GAsyncResult *result,
               gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  int response;
  g_autofree char *ret = NULL;
  g_autofree char *uri = NULL;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  uri = g_key_file_get_string (keyfile, "result", "uri", NULL);

  ret = xdp_portal_take_screenshot_finish (portal, result, &error);

  if (response == 0)
    {
      g_assert_no_error (error);
      g_assert_cmpstr (ret, ==, uri);
    }
  else if (response == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (response == 2)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

void
test_screenshot_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_string (keyfile, "result", "uri", "file://test/image");
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_take_screenshot (portal, NULL, 0, NULL, screenshot_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* test that everything works as expected when the
 * backend takes some time to send its response, as
 * is to be expected from a real backend that presents
 * dialogs to the user.
 */
void
test_screenshot_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_screenshot_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_string (keyfile, "result", "uri", "file://test/image");

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_take_screenshot (portal, NULL, 0, NULL, screenshot_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test that user cancellation works as expected.
 * We simulate that the user cancels a hypothetical dialog,
 * by telling the backend to return 1 as response code.
 * And we check that we get the expected G_IO_ERROR_CANCELLED.
 */
void
test_screenshot_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_screenshot_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_take_screenshot (portal, NULL, 0, NULL, screenshot_cb, keyfile);

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

/* Test that app-side cancellation works as expected.
 * We cancel the cancellable while while the hypothetical
 * dialog is up, and tell the backend that it should
 * expect a Close call. We rely on the backend to
 * verify that that call actually happened.
 */
void
test_screenshot_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GCancellable) cancellable = NULL;

  reset_screenshot_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = 0;
  xdp_portal_take_screenshot (portal, NULL, 0, cancellable, screenshot_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_screenshot_parallel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_screenshot_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_string (keyfile, "result", "uri", "file://test/image");
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_take_screenshot (portal, NULL, 0, NULL, screenshot_cb, keyfile);
  xdp_portal_take_screenshot (portal, NULL, 0, NULL, screenshot_cb, keyfile);
  xdp_portal_take_screenshot (portal, NULL, 0, NULL, screenshot_cb, keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);
}

/* Tests for PickColor below */

static void
pick_color_cb (GObject *obj,
               GAsyncResult *result,
               gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  int response;
  g_autoptr(GVariant) ret = NULL;
  double red, green, blue;
  g_autoptr(GVariant) expected = NULL;

  red = g_key_file_get_double (keyfile, "result", "red", NULL);
  green = g_key_file_get_double (keyfile, "result", "green", NULL);
  blue = g_key_file_get_double (keyfile, "result", "blue", NULL);
  expected = g_variant_new ("(ddd)", red, green, blue);

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  ret = xdp_portal_pick_color_finish (portal, result, &error);

  if (response == 0)
    {
      g_assert_no_error (error);
      g_assert_true (g_variant_equal (ret, expected));
    }
  else if (response == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (response == 2)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

void
test_color_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_double (keyfile, "result", "red", 0.3);
  g_key_file_set_double (keyfile, "result", "green", 0.5);
  g_key_file_set_double (keyfile, "result", "blue", 0.7);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* test that everything works as expected when the
 * backend takes some time to send its response, as
 * is to be expected from a real backend that presents
 * dialogs to the user.
 */
void
test_color_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_double (keyfile, "result", "red", 0.2);
  g_key_file_set_double (keyfile, "result", "green", 0.3);
  g_key_file_set_double (keyfile, "result", "blue", 0.4);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test that user cancellation works as expected.
 * We simulate that the user cancels a hypothetical dialog,
 * by telling the backend to return 1 as response code.
 * And we check that we get the expected G_IO_ERROR_CANCELLED.
 */
void
test_color_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test that app-side cancellation works as expected.
 * We cancel the cancellable while while the hypothetical
 * dialog is up, and tell the backend that it should
 * expect a Close call. We rely on the backend to
 * verify that that call actually happened.
 */
void
test_color_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GCancellable) cancellable = NULL;

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = 0;
  xdp_portal_pick_color (portal, NULL, cancellable, pick_color_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_color_parallel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  set_screenshot_permissions ("no");

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_double (keyfile, "result", "red", 0.3);
  g_key_file_set_double (keyfile, "result", "green", 0.5);
  g_key_file_set_double (keyfile, "result", "blue", 0.7);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);
}
