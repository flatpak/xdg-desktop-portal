#include <config.h>

#include "openuri.h"

#include <libportal/portal.h>
#include "src/xdp-utils.h"
#include "src/xdp-impl-dbus.h"

extern XdpImplLockdown *lockdown;

extern char outdir[];

static int got_info = 0;

static void
open_uri_cb (GObject *obj,
             GAsyncResult *result,
             gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  gboolean ret;
  int response;
  int domain;
  int code;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  domain = g_key_file_get_integer (keyfile, "result", "error_domain", NULL);
  code = g_key_file_get_integer (keyfile, "result", "error_code", NULL);

  ret = xdp_portal_open_uri_finish (portal, result, &error);
  if (response == 0)
    {
      g_assert_no_error (error);
      g_assert_true (ret);
    }
  else if (response == 1)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_false (ret);
    }
  else if (response == 2)
    {
      g_assert_error (error, domain, code);
      g_assert_false (ret);
    }
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

void
test_open_uri_http (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_boolean (keyfile, "backend", "expect-no-call", 1);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, "http://www.flatpak.org", FALSE, NULL, open_uri_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_uri_file (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  g_free (path);
  path = g_build_filename (outdir, "test.txt", NULL);
  g_file_set_contents (path, "text", -1, &error);
  g_assert_no_error (error);

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, uri, FALSE, NULL, open_uri_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_uri_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  g_free (path);
  path = g_build_filename (outdir, "test.txt", NULL);
  g_file_set_contents (path, "text", -1, &error);
  g_assert_no_error (error);

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, uri, FALSE, NULL, open_uri_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_uri_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  g_free (path);
  path = g_build_filename (outdir, "test.txt", NULL);
  g_file_set_contents (path, "text", -1, &error);
  g_assert_no_error (error);

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, uri, FALSE, NULL, open_uri_cb, keyfile);

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
test_open_uri_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  GCancellable *cancellable;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  g_free (path);
  path = g_build_filename (outdir, "test.txt", NULL);
  g_file_set_contents (path, "text", -1, &error);
  g_assert_no_error (error);

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, uri, FALSE, cancellable, open_uri_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_uri_lockdown (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  xdp_impl_lockdown_set_disable_application_handlers (lockdown, TRUE);

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_integer (keyfile, "result", "error_domain", XDG_DESKTOP_PORTAL_ERROR);
  g_key_file_set_integer (keyfile, "result", "error_code", XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, "http://www.flatpak.org", FALSE, NULL, open_uri_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_impl_lockdown_set_disable_application_handlers (lockdown, FALSE);
}
