#include <config.h>

#include "print.h"

#include <libportal/portal.h>
#include "src/xdp-utils.h"
#include "src/xdp-impl-dbus.h"

extern char outdir[];

static gboolean got_info = FALSE;

static void
prepare_cb (GObject *obj,
            GAsyncResult *result,
            gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;
  GKeyFile *keyfile = data;
  int response;
  int domain;
  int code;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  domain = g_key_file_get_integer (keyfile, "result", "error_domain", NULL);
  code = g_key_file_get_integer (keyfile, "result", "error_code", NULL);

  ret = xdp_portal_prepare_print_finish (portal, result, &error);
  if (response == 0)
    {
      int expected, token;

      g_assert_no_error (error);

      expected = g_key_file_get_integer (keyfile, "result", "token", NULL);
      g_variant_lookup (ret, "token", "u", &token);

      g_assert_cmpint (expected, ==, token);
    }
  else if (response == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (response == 2)
    g_assert_error (error, domain, code);
  else
    g_assert_not_reached ();

  got_info = TRUE;

  g_main_context_wakeup (NULL);
}

void
test_prepare_print_libportal (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_prepare_print (portal, NULL, "test", FALSE, NULL, NULL, NULL, prepare_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_prepare_print_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_prepare_print (portal, NULL, "test", FALSE, NULL, NULL, NULL, prepare_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_prepare_print_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_prepare_print (portal, NULL, "test", FALSE, NULL, NULL, NULL, prepare_cb, keyfile);

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
test_prepare_print_close (void)
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

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = FALSE;
  xdp_portal_prepare_print (portal, NULL, "test", FALSE, NULL, NULL, cancellable, prepare_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Test"
#define BACKEND_OBJECT_PATH "/org/freedesktop/portal/desktop"

void
test_prepare_print_lockdown (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GDBusProxy) lockdown = NULL;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  lockdown = G_DBUS_PROXY (xdp_impl_lockdown_proxy_new_sync (session_bus,
                                                             0,
                                                             BACKEND_BUS_NAME,
                                                             BACKEND_OBJECT_PATH,
                                                             NULL,
                                                             &error));
  g_assert_no_error (error);

  xdp_impl_lockdown_set_disable_printing (XDP_IMPL_LOCKDOWN (lockdown), TRUE);

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_integer (keyfile, "result", "error_domain", XDG_DESKTOP_PORTAL_ERROR);
  g_key_file_set_integer (keyfile, "result", "error_code", XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_prepare_print (portal, NULL, "test", FALSE, NULL, NULL, NULL, prepare_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_impl_lockdown_set_disable_printing (XDP_IMPL_LOCKDOWN (lockdown), FALSE);
}

void
test_prepare_print_results (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);
  g_key_file_set_integer (keyfile, "result", "token", 123);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_prepare_print (portal, NULL, "test", FALSE, NULL, NULL, NULL, prepare_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* test of Print below */

static void
print_cb (GObject *obj,
          GAsyncResult *result,
          gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  int response;
  int domain;
  int code;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  domain = g_key_file_get_integer (keyfile, "result", "error_domain", NULL);
  code = g_key_file_get_integer (keyfile, "result", "error_code", NULL);

  xdp_portal_print_file_finish (portal, result, &error);
  if (response == 0)
    {
      g_assert_no_error (error);
    }
  else if (response == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (response == 2)
    g_assert_error (error, domain, code);
  else
    g_assert_not_reached ();

  got_info = TRUE;

  g_main_context_wakeup (NULL);
}

void
test_print_libportal (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_print_file (portal, NULL, "test", FALSE, 0, path, NULL, print_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_print_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_print_file (portal, NULL, "test", FALSE, 0, path, NULL, print_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_print_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = FALSE;
  xdp_portal_print_file (portal, NULL, "test", FALSE, 0, path, NULL, print_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_print_close (void)
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

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = FALSE;
  xdp_portal_print_file (portal, NULL, "test", FALSE, 0, path, cancellable, print_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_print_lockdown (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GDBusProxy) lockdown = NULL;
  g_autoptr(GCancellable) cancellable = NULL;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  lockdown = G_DBUS_PROXY (xdp_impl_lockdown_proxy_new_sync (session_bus,
                                                             0,
                                                             BACKEND_BUS_NAME,
                                                             BACKEND_OBJECT_PATH,
                                                             NULL,
                                                             &error));
  g_assert_no_error (error);

  xdp_impl_lockdown_set_disable_printing (XDP_IMPL_LOCKDOWN (lockdown), TRUE);

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_integer (keyfile, "result", "error_domain", XDG_DESKTOP_PORTAL_ERROR);
  g_key_file_set_integer (keyfile, "result", "error_code", XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED);

  path = g_build_filename (outdir, "print", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = FALSE;
  xdp_portal_print_file (portal, NULL, "test", FALSE, 0, path, cancellable, print_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_impl_lockdown_set_disable_printing (XDP_IMPL_LOCKDOWN (lockdown), FALSE);
}
