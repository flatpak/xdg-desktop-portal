#include <config.h>

#include "screenshot.h"

#include <libportal/portal.h>

extern char outdir[];

static gboolean got_info;

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
  g_autoptr(GVariant) retv = NULL;
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

  got_info = TRUE;

  g_main_context_wakeup (NULL);
}

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

  got_info = TRUE;

  g_main_context_wakeup (NULL);
}

void
test_screenshot_libportal (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_string (keyfile, "result", "uri", "file://test/image");
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "screenshot", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_take_screenshot (portal, NULL, FALSE, FALSE, NULL, screenshot_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_screenshot_color (void)
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

  got_info = FALSE;
  xdp_portal_pick_color (portal, NULL, NULL, pick_color_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}
