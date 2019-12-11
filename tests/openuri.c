#include <config.h>

#include "openuri.h"

#include <libportal/portal.h>
#include "src/xdp-utils.h"
#include "src/xdp-impl-dbus.h"

extern XdpImplLockdown *lockdown;
extern XdpImplPermissionStore *permission_store;

extern char outdir[];

static int got_info = 0;

static void
set_openuri_permissions (const char *type,
                         const char *handler,
                         guint count,
                         guint threshold)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *count_s = g_strdup_printf ("%u", count);
  g_autofree char *threshold_s = g_strdup_printf ("%u", threshold);
  const char *permissions[4];

  permissions[0] = handler;
  permissions[1] = count_s;
  permissions[2] = threshold_s;
  permissions[3] = NULL;

  xdp_impl_permission_store_call_delete_sync (permission_store,
                                              "desktop-used-apps",
                                              type,
                                              NULL,
                                              NULL);

  xdp_impl_permission_store_call_set_permission_sync (permission_store,
                                                      "desktop-used-apps",
                                                      TRUE,
                                                      type,
                                                      "",
                                                      permissions,
                                                      NULL,
                                                      &error);
  g_assert_no_error (error);
}

static void
unset_openuri_permissions (const char *type)
{
  xdp_impl_permission_store_call_delete_sync (permission_store,
                                              "desktop-used-apps",
                                              type,
                                              NULL,
                                              NULL);
  /* Ignore the error here, since this fails if the table doesn't exist */
}

static void
enable_paranoid_mode (const char *type)
{
  GVariantBuilder data_builder;

  /* turn on paranoid mode to ensure we get a backend call */
  g_variant_builder_init (&data_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&data_builder, "{sv}", "always-ask", g_variant_new_boolean (TRUE));
  xdp_impl_permission_store_call_set_value_sync (permission_store,
                                                 "desktop-used-apps",
                                                 TRUE,
                                                 type,
                                                 g_variant_new_variant (g_variant_builder_end (&data_builder)),
                                                 NULL,
                                                 NULL);
}

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

  unset_openuri_permissions ("x-scheme-handler/http");
  enable_paranoid_mode ("x-scheme-handler/http");

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "appchooser", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_open_uri (portal, NULL, "http://www.flatpak.org", FALSE, FALSE, NULL, open_uri_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_open_uri_http2 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GAppInfo) app = NULL;
  g_autofree char *app_id = NULL;

  app = g_app_info_get_default_for_type ("x-scheme-handler/http", FALSE);

  if (app == NULL)
    {
      g_test_skip ("No default handler for x-scheme-handler/http set");
      return;
    }

  app_id = g_strndup (g_app_info_get_id (app), strlen (g_app_info_get_id (app)) - strlen (".desktop"));

  unset_openuri_permissions ("text/plain");
  set_openuri_permissions ("x-scheme-handler/http", app_id, 3, 3);

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
  xdp_portal_open_uri (portal, NULL, "http://www.flatpak.org", FALSE, FALSE, NULL, open_uri_cb, keyfile);

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

  unset_openuri_permissions ("text/plain");
  enable_paranoid_mode ("x-scheme-handler/http");

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
  xdp_portal_open_uri (portal, NULL, uri, FALSE, FALSE, NULL, open_uri_cb, keyfile);

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

  unset_openuri_permissions ("text/plain");
  enable_paranoid_mode ("x-scheme-handler/http");

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
  xdp_portal_open_uri (portal, NULL, uri, FALSE, FALSE, NULL, open_uri_cb, keyfile);

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

  unset_openuri_permissions ("text/plain");
  enable_paranoid_mode ("text/plain");

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
  xdp_portal_open_uri (portal, NULL, uri, FALSE, FALSE, NULL, open_uri_cb, keyfile);

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

  unset_openuri_permissions ("text/plain");
  enable_paranoid_mode ("x-scheme-handler/http");

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
  xdp_portal_open_uri (portal, NULL, uri, FALSE, FALSE, cancellable, open_uri_cb, keyfile);

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
  xdp_portal_open_uri (portal, NULL, "http://www.flatpak.org", FALSE, FALSE, NULL, open_uri_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_impl_lockdown_set_disable_application_handlers (lockdown, FALSE);
}

static void
open_dir_cb (GObject *obj,
             GAsyncResult *result,
             gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  gboolean ret;
  int response;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);

  ret = xdp_portal_open_directory_finish (portal, result, &error);
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
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_false (ret);
    }
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

void
test_open_directory (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  g_autoptr(GAppInfo) app = NULL;

  keyfile = g_key_file_new ();

  app = g_app_info_get_default_for_type ("inode/directory", FALSE);

  if (app == NULL)
    {
      g_test_skip ("No default handler for inode/directory set");
      return;
    }

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", app != NULL ? 0 : 2);

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
  xdp_portal_open_directory (portal, NULL, uri, NULL, open_dir_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}
