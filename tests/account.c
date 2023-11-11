#include <config.h>

#include "account.h"

#include <libportal/portal.h>

extern char outdir[];

/* We use g_main_context_wakeup() and a boolean variable
 * to make the test cases wait for async calls to return
 * without a maze of callbacks.
 *
 * The tests communicate with the backend via a keyfile
 * in a shared location.
 */
static int got_info = 0;

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
      free (t);

      t = g_key_file_get_string (keyfile, "account", "name", NULL);
      res = g_variant_lookup (ret, "name", "&s", &s); 
      g_assert (res == (t != NULL));
      if (t) g_assert_cmpstr (s, ==, t);
      free (t);

      t = g_key_file_get_string (keyfile, "account", "image", NULL);
      res = g_variant_lookup (ret, "image", "&s", &s); 
      g_assert (res == (t != NULL));
      if (t) g_assert_cmpstr (s, ==, t);
      free (t);
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

  got_info++;
  g_main_context_wakeup (NULL);
}

/* some basic tests using libportal, and test that communication
 * with the backend via keyfile works
 */
void
test_account_basic (void)
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

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "test", 0, NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* check that the reason argument makes it to the backend
 */
void
test_account_reason (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char *long_reason;

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

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "xx", 0, NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "yy", 0, NULL, account_cb_fail, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  g_key_file_remove_key (keyfile, "backend", "reason", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  long_reason = "This reason is unreasonably long, it stretches over "
      "more than twohundredfiftysix characters, which is really quite "
      "long. Excessively so. The portal frontend will silently drop "
      "reasons of this magnitude. If you can't express your reasons "
      "concisely, you probably have no good reason in the first place "
      "and are just waffling around.";
  g_assert (g_utf8_strlen (long_reason, -1) > 256);

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, long_reason, 0, NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

}

/* test that everything works as expected when the
 * backend takes some time to send its response, as
 * is to be expected from a real backend that presents
 * dialogs to the user.
 */
void
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

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "xx", 0, NULL, account_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test that user cancellation works as expected.
 * We simulate that the user cancels a hypothetical dialog,
 * by telling the backend to return 1 as response code.
 * And we check that we get the expected G_IO_ERROR_CANCELLED.
 */
void
test_account_cancel (void)
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
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "xx", 0, NULL, account_cb, keyfile);

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
test_account_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GCancellable) cancellable = NULL;

  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "account", "id", "test");
  g_key_file_set_string (keyfile, "account", "name", "Donald Duck");
  g_key_file_set_string (keyfile, "backend", "reason", "xx");
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "xx", 0, cancellable, account_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test multiple requests in parallel */
void
test_account_parallel (void)
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
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "account", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_get_user_information (portal, NULL, "test", 0, NULL, account_cb, keyfile);
  xdp_portal_get_user_information (portal, NULL, "test", 0, NULL, account_cb, keyfile);
  xdp_portal_get_user_information (portal, NULL, "test", 0, NULL, account_cb, keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);
}

