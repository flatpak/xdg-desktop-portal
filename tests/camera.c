#include <config.h>

#include "camera.h"

#include <libportal/portal.h>
#include "xdp-utils.h"
#include "xdp-impl-dbus.h"

#include "utils.h"

extern char outdir[];

static int got_info;

extern XdpDbusImplPermissionStore *permission_store;
extern XdpDbusImplLockdown *lockdown;
extern gchar *appid;

static void
set_camera_permissions (const char *permission)
{
  const char *permissions[2] = { NULL, NULL };
  g_autoptr(GError) error = NULL;

  permissions[0] = permission;
  xdp_dbus_impl_permission_store_call_set_permission_sync (permission_store,
                                                           "devices",
                                                           TRUE,
                                                           "camera",
                                                           appid,
                                                           permissions,
                                                           NULL,
                                                           &error);
  g_assert_no_error (error);
}

static void
reset_camera_permissions (void)
{
  set_camera_permissions (NULL);
}

static void
camera_cb (GObject *obj,
           GAsyncResult *result,
           gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  int response;
  int domain;
  int code;
  gboolean ret;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  domain = g_key_file_get_integer (keyfile, "result", "error_domain", NULL);
  code = g_key_file_get_integer (keyfile, "result", "error_code", NULL);

  ret = xdp_portal_access_camera_finish (portal, result, &error);

  g_debug ("camera cb: %d", g_key_file_get_integer (keyfile, "result", "marker", NULL));
  if (response == 0)
    {
      g_assert_true (ret);
      g_assert_no_error (error);
    }
  else if (response == 1)
    {
      g_assert_false (ret);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }
  else if (response == 2)
    {
      g_assert_false (ret);
      g_assert_error (error, domain, code);
    }
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

void
test_camera_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_camera_permissions ();

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_camera_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_camera_permissions ();

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "result", "marker", 1);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_camera_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_camera_permissions ();

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "result", "marker", 2);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

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
test_camera_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GCancellable) cancellable = NULL;

  reset_camera_permissions ();

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "result", "marker", 3);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  //g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, cancellable, camera_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_camera_lockdown (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_camera_permissions ();

  tests_set_property_sync (G_DBUS_PROXY (lockdown),
                           "org.freedesktop.impl.portal.Lockdown",
                           "disable-camera",
                           g_variant_new_boolean (TRUE),
                           &error);
  g_assert_no_error (error);

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "result", "marker", 4);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_integer (keyfile, "result", "error_domain", XDG_DESKTOP_PORTAL_ERROR);
  g_key_file_set_integer (keyfile, "result", "error_code", XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  tests_set_property_sync (G_DBUS_PROXY (lockdown),
                           "org.freedesktop.impl.portal.Lockdown",
                           "disable-camera",
                           g_variant_new_boolean (FALSE),
                           &error);
  g_assert_no_error (error);
}

/* Test the effect of the user denying the access dialog */
void
test_camera_no_access1 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_camera_permissions ();

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 2);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

/* Test the effect of the permissions being stored */
void
test_camera_no_access2 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  set_camera_permissions ("no");

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_camera_parallel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  reset_camera_permissions ();

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);
  xdp_portal_access_camera (portal, NULL, 0, NULL, camera_cb, keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);
}

