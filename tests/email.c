#include <config.h>

#include "email.h"

#include <libportal/portal.h>
#include "src/xdp-utils.h"

extern char outdir[];

static int got_info;

static void
email_cb (GObject *obj,
          GAsyncResult *result,
          gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  gboolean ret;
  GKeyFile *keyfile = data;
  int response;
  int domain;
  int code;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  domain = g_key_file_get_integer (keyfile, "result", "error_domain", NULL);
  code = g_key_file_get_integer (keyfile, "result", "error_code", NULL);

  ret = xdp_portal_compose_email_finish (portal, result, &error);
  g_assert (ret == (response == 0));
  if (response == 0)
    g_assert_no_error (error);
  else if (response == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (response == 2)
    g_assert_error (error, domain, code);
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

/* some basic tests using libportal, and test that communication
 * with the backend via keyfile works
 */
void
test_email_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "input", "address", "mclasen@redhat.com");
  g_key_file_set_string (keyfile, "input", "subject", "Re: portal tests");
  g_key_file_set_string (keyfile, "input", "body", "You have to see this...");

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            "mclasen@redhat.com",
                            "Re: portal tests",
                            "You have to see this...",
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* test that an invalid address triggers an error
 */
void
test_email_address (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char *address;

  keyfile = g_key_file_new ();

  address = "gibberish! not an email address\n%Q";

  g_key_file_set_string (keyfile, "input", "address", address); 

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_integer (keyfile, "result", "error_domain", XDG_DESKTOP_PORTAL_ERROR);
  g_key_file_set_integer (keyfile, "result", "error_code", XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            address,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* test that an invalid subject triggers an error
 */
void
test_email_subject (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char *subject;

  keyfile = g_key_file_new ();

  subject = "not\na\nvalid\nsubject line";

  g_key_file_set_string (keyfile, "input", "subject", subject); 

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_integer (keyfile, "result", "error_domain", XDG_DESKTOP_PORTAL_ERROR);
  g_key_file_set_integer (keyfile, "result", "error_code", XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            NULL,
                            subject,
                            NULL,
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  subject = "This subject line is too long, much too long. It is more than twohundred characters long, which is much, much too long for a reasonable subject line. Be concise! This is not twitter where you can use hundreds of characters, including Emoji like 😂️ or 😩️";
  g_assert_cmpint (g_utf8_strlen (subject, -1), >, 200);

  g_key_file_set_string (keyfile, "input", "subject", subject); 
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            NULL,
                            subject,
                            NULL,
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* test that everything works as expected when the
 * backend takes some time to send its response, as
 * is to be expected from a real backend that presents
 * dialogs to the user.
 */
void
test_email_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char *address;
  const char *subject;

  address = "mclasen@redhat.com";
  subject = "delay test";

  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "input", "address", address);
  g_key_file_set_string (keyfile, "input", "subject", subject);

  g_key_file_set_integer (keyfile, "backend", "delay", 400);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            address,
                            subject,
                            NULL,
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test that user cancellation works as expected.
 * We simulate that the user cancels a hypothetical dialog,
 * by telling the backend to return 1 as response code.
 * And we check that we get the expected G_IO_ERROR_CANCELLED.
 */
void
test_email_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  const char *address;
  const char *subject;

  address = "mclasen@redhat.com";
  subject = "delay test";

  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "input", "address", address);
  g_key_file_set_string (keyfile, "input", "subject", subject);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            address,
                            subject,
                            NULL,
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

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
test_email_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GCancellable) cancellable = NULL;
  const char *address;
  const char *subject;

  address = "mclasen@redhat.com";
  subject = "delay test";

  keyfile = g_key_file_new ();
  g_key_file_set_string (keyfile, "input", "address", address);
  g_key_file_set_string (keyfile, "input", "subject", subject);

  g_key_file_set_integer (keyfile, "backend", "delay", 400);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            address,
                            subject,
                            NULL,
                            NULL,
                            cancellable,
                            email_cb,
                            keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_email_parallel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "input", "address", "mclasen@redhat.com");
  g_key_file_set_string (keyfile, "input", "subject", "Re: portal tests");
  g_key_file_set_string (keyfile, "input", "body", "You have to see this...");

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "email", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_compose_email (portal, NULL,
                            "mclasen@redhat.com",
                            "Re: portal tests",
                            "You have to see this...",
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);
  xdp_portal_compose_email (portal, NULL,
                            "mclasen@redhat.com",
                            "Re: portal tests",
                            "You have to see this...",
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);
  xdp_portal_compose_email (portal, NULL,
                            "mclasen@redhat.com",
                            "Re: portal tests",
                            "You have to see this...",
                            NULL,
                            NULL,
                            email_cb,
                            keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);
}

