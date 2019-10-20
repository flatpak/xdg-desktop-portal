#include <config.h>

#include "account.h"

#include <libportal/portal.h>

extern char outdir[];

static gboolean got_info = FALSE;

static void
file_chooser_cb (GObject *obj,
                 GAsyncResult *result,
                 gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;
  GKeyFile *keyfile = data;
  int response;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);

  ret = xdp_portal_open_file_finish (portal, result, &error);
  if (response == 0)
    {
      const char * const *uris;
      g_auto(GStrv) expected = NULL;

      g_assert_no_error (error);
      g_variant_lookup (ret, "uris", "^a&s", &uris);
      expected = g_key_file_get_string_list (keyfile, "result", "uris", NULL, NULL);

      g_assert (g_strv_equal (uris, (const char * const *)expected)); 
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
test_open_file_libportal (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char * uris[] = {
    "file:///test/file",
    NULL
  };

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_string_list (keyfile, "result", "uris", uris, g_strv_length (uris));

  path = g_build_filename (outdir, "filechooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_open_file (portal, NULL, "test", FALSE, FALSE, NULL, NULL, NULL, file_chooser_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_file_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char * uris[] = {
    "file:///test/file",
    NULL
  };

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_string_list (keyfile, "result", "uris", uris, g_strv_length (uris));

  path = g_build_filename (outdir, "filechooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_open_file (portal, NULL, "test", FALSE, FALSE, NULL, NULL, NULL, file_chooser_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_file_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char * uris[] = {
    "file:///test/file",
    NULL
  };

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);
  g_key_file_set_string_list (keyfile, "result", "uris", uris, g_strv_length (uris));

  path = g_build_filename (outdir, "filechooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_open_file (portal, NULL, "test", FALSE, FALSE, NULL, NULL, NULL, file_chooser_cb, keyfile);

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

void
test_open_file_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancellable = NULL;
  g_autofree char *path = NULL;
  const char * uris[] = {
    "file:///test/file",
    NULL
  };

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 1);
  g_key_file_set_string_list (keyfile, "result", "uris", uris, g_strv_length (uris));

  path = g_build_filename (outdir, "filechooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = FALSE;
  xdp_portal_open_file (portal, NULL, "test", FALSE, FALSE, NULL, NULL, cancellable, file_chooser_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

