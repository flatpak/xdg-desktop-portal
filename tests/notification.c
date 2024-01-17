
#include <config.h>

#include "account.h"

#include <libportal/portal.h>
#include "xdp-utils.h"

extern char outdir[];

static int got_info;

static void
notification_succeed (GObject *source,
                      GAsyncResult *result,
                      gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (source);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_add_notification_finish (portal, result, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
notification_fail (GObject *source,
                   GAsyncResult *result,
                   gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (source);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_add_notification_finish (portal, result, &error);
  g_assert_false (res);
  g_assert_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT);

  got_info++;
  g_main_context_wakeup (NULL);
}

static void
notification_action_invoked (XdpPortal *portal,
                             const char *id,
                             const char *action,
                             GVariant *parameter,
                             gpointer data)
{
  GKeyFile *keyfile = data;
  g_autofree char *exp_id = NULL;
  g_autofree char *exp_action = NULL;

  exp_id = g_key_file_get_string (keyfile, "notification", "id", NULL);
  exp_action = g_key_file_get_string (keyfile, "notification", "action", NULL);

  g_assert_cmpstr (exp_id, ==, id);
  g_assert_cmpstr (exp_action, ==, action);

  got_info++;

  g_main_context_wakeup (NULL);
}

static void
run_notification_test (const char *notification_id,
                       const char *notification_s,
                       const char *exp_notification_s,
                       gboolean    exp_fail)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  gulong id;

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, NULL);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", exp_notification_s ? exp_notification_s : notification_s);
  g_key_file_set_string (keyfile, "notification", "id", notification_id);
  g_key_file_set_string (keyfile, "notification", "action", "test-action");

  if (exp_fail)
    g_key_file_set_boolean (keyfile, "backend", "expect-no-call", TRUE);
  else
    g_key_file_set_integer (keyfile, "backend", "delay", 200);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  if (!exp_fail)
    id = g_signal_connect (portal, "notification-action-invoked", G_CALLBACK (notification_action_invoked), keyfile);

  got_info = 0;

  if (exp_fail)
    xdp_portal_add_notification (portal, notification_id, notification, 0, NULL, notification_fail, NULL);
  else
    xdp_portal_add_notification (portal, notification_id, notification, 0, NULL, notification_succeed, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  if (!exp_fail)
    g_signal_handler_disconnect (portal, id);

  xdp_portal_remove_notification (portal, "test");
}

void
test_notification_basic (void)
{
  const char *notification_s;

  notification_s = "{ 'title': <'title'>, "
                   "  'body': <'test notification body'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test ("test1", notification_s, NULL, FALSE);
}

void
test_notification_buttons (void)
{
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 2'>, "
                   "  'body': <'test notification body 2'>, "
                   "  'priority': <'low'>, "
                   "  'default-action': <'test-action'>, "
                   "  'buttons': <[{'label': <'button1'>, 'action': <'action1'>}, "
                   "               {'label': <'button2'>, 'action': <'action2'>}]> "
                   "}";

  run_notification_test ("test2", notification_s, NULL, FALSE);
}

void
test_notification_bad_arg (void)
{
  const char *notification_s;
  const char *exp_notification_s;

  notification_s = "{ 'title': <'test notification 3'>, "
                   "  'bodx': <'test notification body 3'> "
                   "}";

  exp_notification_s = "{ 'title': <'test notification 3'> }";

  run_notification_test ("test3", notification_s, exp_notification_s, FALSE);
}

void
test_notification_bad_priority (void)
{
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 2'>, "
                   "  'body': <'test notification body 2'>, "
                   "  'priority': <'invalid'> "
                   "}";

  run_notification_test ("test4", notification_s, NULL, TRUE);
}

void
test_notification_bad_button (void)
{
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'buttons': <[{'labex': <'button1'>, 'action': <'action1'>}, "
                   "               {'label': <'button2'>, 'action': <'action2'>}]> "
                   "}";

  run_notification_test ("test5", notification_s, NULL, TRUE);
}
