#include <config.h>

#include "inhibit.h"

#include <libportal/portal.h>
#include "xdp-impl-dbus.h"

extern char outdir[];

extern XdpDbusImplPermissionStore *permission_store;

static void
set_inhibit_permissions (const char **permissions)
{
  g_autoptr(GError) error = NULL;

  xdp_dbus_impl_permission_store_call_set_permission_sync (permission_store,
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
  xdp_dbus_impl_permission_store_call_delete_sync (permission_store,
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
      g_assert_no_error (error);
      g_assert_cmpint (id, >, 0);
    }
  else
    {
      g_assert_nonnull (error);
      g_assert_cmpint (id, ==, -1);
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

  flags = XDP_INHIBIT_FLAG_LOGOUT|XDP_INHIBIT_FLAG_USER_SWITCH;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);

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

  flags = XDP_INHIBIT_FLAG_USER_SWITCH|XDP_INHIBIT_FLAG_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);

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

  flags = XDP_INHIBIT_FLAG_USER_SWITCH|XDP_INHIBIT_FLAG_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);

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

  flags = XDP_INHIBIT_FLAG_USER_SWITCH|XDP_INHIBIT_FLAG_IDLE;

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
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, cancellable, inhibit_cb, keyfile);

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

  flags = XDP_INHIBIT_FLAG_LOGOUT|XDP_INHIBIT_FLAG_USER_SWITCH;

  g_key_file_set_integer (keyfile, "inhibit", "flags", XDP_INHIBIT_FLAG_LOGOUT); /* user switch is not allowed */

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);

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

  flags = XDP_INHIBIT_FLAG_USER_SWITCH|XDP_INHIBIT_FLAG_IDLE;

  g_key_file_set_integer (keyfile, "inhibit", "flags", flags);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);
  xdp_portal_session_inhibit (portal, NULL, "Testing portals", flags, NULL, inhibit_cb, keyfile);

  while (got_info < 3)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_uninhibit (portal, inhibit_id[0]);
  xdp_portal_session_uninhibit (portal, inhibit_id[1]);
  xdp_portal_session_uninhibit (portal, inhibit_id[2]);
}

/* tests below test session state monitoring */

static void
monitor_cb (GObject *object,
            GAsyncResult *result,
            gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  g_autoptr(GError) error = NULL;
  gboolean ret;

  ret = xdp_portal_session_monitor_start_finish (portal, result, &error);
  g_assert_true (ret);
  g_assert_no_error (error);

  got_info += 1;
}

static void
session_state_changed_cb (XdpPortal *portal,
                          gboolean screensaver_active,
                          XdpLoginSessionState state,
                          gpointer data)
{
  g_assert_false (screensaver_active);
  g_assert_cmpint (state, ==, XDP_LOGIN_SESSION_RUNNING);

  got_info += 1;
}

static void
session_state_changed_cb2 (XdpPortal *portal,
                           gboolean screensaver_active,
                           XdpLoginSessionState state,
                           gpointer data)
{
  g_assert_false (screensaver_active);
  g_assert_cmpint (state, ==, XDP_LOGIN_SESSION_QUERY_END);

  got_info += 1;
}

static gboolean
bump_got_info (gpointer data)
{
  got_info += 1;

  return G_SOURCE_REMOVE;
}

void
test_inhibit_monitor (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  gulong id;

  if (g_getenv ("TEST_IN_CI"))
    {
      g_test_skip ("Skip tests that are unreliable in CI");
      return;
    }

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 1000);
  g_key_file_set_string (keyfile, "backend", "change", "query-end");

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  id = g_signal_connect (portal, "session-state-changed", G_CALLBACK (session_state_changed_cb), NULL);

  got_info = 0;
  xdp_portal_session_monitor_start (portal, NULL, 0, NULL, monitor_cb, NULL);

  /* we get a monitor_cb and an initial state-changed emission */
  while (got_info < 2)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (portal, id);

  /* now wait for the query-end state */
  g_debug ("waiting for query-end state\n");
  got_info = 0;
  g_signal_connect (portal, "session-state-changed", G_CALLBACK (session_state_changed_cb2), NULL);

  while (got_info < 1)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_monitor_stop (portal);

  /* after calling stop, no more state-changed signals */
  got_info = 0;
  g_timeout_add (500, bump_got_info, NULL);
  while (got_info < 1)
    g_main_context_iteration (NULL, TRUE);
}
