#include <config.h>

#include "inhibit.h"

#include <libportal/portal.h>
#include "src/xdp-impl-dbus.h"

extern char outdir[];

extern XdpImplPermissionStore *permission_store;

static void
set_inhibit_permissions (const char **permissions)
{
  g_autoptr(GError) error = NULL;

  xdp_impl_permission_store_call_set_permission_sync (permission_store,
                                                      "inhibit",
                                                      TRUE,
                                                      "inhibit",
                                                      "",
                                                      permissions,
                                                      NULL,
                                                      &error);
  g_assert_no_error (error);
}

static void
unset_inhibit_permissions (void)
{
  xdp_impl_permission_store_call_delete_sync (permission_store,
                                              "inhibit",
                                              "inhibit",
                                              NULL,
                                              NULL);
  /* Ignore the error here, since this fails if the table doesn't exist */
}

static int got_info;
static int inhibit_id[3];

static void
inhibit_cb (GObject *object,
            GAsyncResult *result,
            gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  GKeyFile *keyfile = data;
  g_autoptr(GError) error = NULL;
  int response;
  int id;

  g_debug ("Got inhibit callback");

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);

  id = xdp_portal_session_inhibit_finish (portal, result, &error);

  if (response == 0)
    {
      g_assert_cmpint (id, >, 0);
      g_assert_no_error (error);
    }
  else
    {
      g_assert_cmpint (id, ==, -1);
      g_assert_nonnull (error);
    }

  g_assert (0 <= got_info && got_info < 3);
  inhibit_id[got_info] = id;

  got_info++;
  g_main_context_wakeup (NULL);
}

void
test_inhibit_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  XdpInhibitFlags flags;
  const char *perms[] = { "logout", "suspend", NULL };

  set_inhibit_permissions (perms);
  unset_inhibit_permissions ();

  keyfile = g_key_file_new ();

  flags = XDP_INHIBIT_LOGOUT|XDP_INHIBIT_USER_SWITCH;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_uninhibit (portal, inhibit_id[0]);
}

void
test_inhibit_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  XdpInhibitFlags flags;

  unset_inhibit_permissions ();

  keyfile = g_key_file_new ();

  flags = XDP_INHIBIT_USER_SWITCH|XDP_INHIBIT_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_uninhibit (portal, inhibit_id[0]);
}

void
test_inhibit_cancel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  XdpInhibitFlags flags;

  unset_inhibit_permissions ();

  keyfile = g_key_file_new ();

  flags = XDP_INHIBIT_USER_SWITCH|XDP_INHIBIT_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);

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
test_inhibit_close (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  XdpInhibitFlags flags;
  g_autoptr(GCancellable) cancellable = NULL;

  unset_inhibit_permissions ();

  keyfile = g_key_file_new ();

  flags = XDP_INHIBIT_USER_SWITCH|XDP_INHIBIT_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);
  g_key_file_set_boolean (keyfile, "backend", "expect-close", 1);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  cancellable = g_cancellable_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", cancellable, inhibit_cb, keyfile);

  g_timeout_add (100, cancel_call, cancellable);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_inhibit_permissions (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  XdpInhibitFlags flags;
  const char *permissions[] = { "logout", "suspend", NULL };

  set_inhibit_permissions (permissions);

  keyfile = g_key_file_new ();

  flags = XDP_INHIBIT_LOGOUT|XDP_INHIBIT_USER_SWITCH;

  g_key_file_set_integer (keyfile, "inhibit", "flags", XDP_INHIBIT_LOGOUT); /* user switch is not allowed */

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_uninhibit (portal, inhibit_id[0]);

  unset_inhibit_permissions ();
}

void
test_inhibit_parallel (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  XdpInhibitFlags flags;

  unset_inhibit_permissions ();

  keyfile = g_key_file_new ();

  flags = XDP_INHIBIT_USER_SWITCH|XDP_INHIBIT_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_uninhibit (portal, inhibit_id[0]);
  xdp_portal_session_uninhibit (portal, inhibit_id[1]);
  xdp_portal_session_uninhibit (portal, inhibit_id[2]);
}
