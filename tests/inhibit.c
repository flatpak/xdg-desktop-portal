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
  g_autoptr(GError) error = NULL;

  xdp_impl_permission_store_call_delete_sync (permission_store,
                                              "inhibit",
                                              "inhibit",
                                              NULL,
                                              &error);
  g_assert_no_error (error);
}

static int got_info;
static int inhibit_id;

static void
inhibit_cb (GObject *object,
            GAsyncResult *result,
            gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  g_autoptr(GError) error = NULL;

  inhibit_id = xdp_portal_session_inhibit_finish (portal, result, &error);
  g_assert_cmpint (inhibit_id, !=, 0);
  g_assert_no_error (error);
    
  got_info++;
  g_main_context_wakeup (NULL);
}

void
test_inhibit_libportal (void)
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

  path = g_build_filename (outdir, "inhibit", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_session_inhibit (portal, NULL, flags, "Testing portals", NULL, inhibit_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_session_uninhibit (portal, inhibit_id);
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

  xdp_portal_session_uninhibit (portal, inhibit_id);

  unset_inhibit_permissions ();
}
