
#include <config.h>

#include "account.h"

#include <libportal/portal.h>
#include "xdp-utils.h"

#define SVG_IMAGE_DATA \
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
  "<svg xmlns=\"http://www.w3.org/2000/svg\" height=\"16px\" width=\"16px\"/>"

extern char outdir[];

static int got_info;

static void
notification_succeed (GObject      *source,
                      GAsyncResult *result,
                      gpointer      data)
{
  g_autoptr(GError) error = NULL;
  XdpPortal *portal;
  gboolean res;

  g_assert_true (XDP_IS_PORTAL (source));
  portal = XDP_PORTAL (source);

  res = xdp_portal_add_notification_finish (portal, result, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
notification_fail (GObject      *source,
                   GAsyncResult *result,
                   gpointer      data)
{
  g_autoptr(GError) error = NULL;
  XdpPortal *portal;
  gboolean res;

  g_assert_true (XDP_IS_PORTAL (source));
  portal = XDP_PORTAL (source);

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
                       const char *expected_notification_s,
                       gboolean    expect_failure)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  gulong id;

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, NULL);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", expected_notification_s ? expected_notification_s : notification_s);
  g_key_file_set_string (keyfile, "notification", "id", notification_id);
  g_key_file_set_string (keyfile, "notification", "action", "test-action");

  if (expect_failure)
    g_key_file_set_boolean (keyfile, "backend", "expect-no-call", TRUE);
  else
    g_key_file_set_integer (keyfile, "backend", "delay", 200);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  if (!expect_failure)
    id = g_signal_connect (portal, "notification-action-invoked", G_CALLBACK (notification_action_invoked), keyfile);

  got_info = 0;

  xdp_portal_add_notification (portal,
                               notification_id,
                               notification,
                               0, NULL,
                               expect_failure ? notification_fail : notification_succeed,
                               NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  if (!expect_failure)
    g_signal_handler_disconnect (portal, id);

  xdp_portal_remove_notification (portal, notification_id);
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
  const char *expected_notification_s;

  notification_s = "{ 'title': <'test notification 3'>, "
                   "  'bodx': <'test notification body 3'> "
                   "}";

  expected_notification_s = "{ 'title': <'test notification 3'> }";

  run_notification_test ("test3", notification_s, expected_notification_s, FALSE);
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

static void
test_icon (const char *serialized_icon,
           const char *expected_serialized_icon,
           gboolean    expect_failure)
{
  g_autofree char *expected_notification_s = NULL;
  g_autofree char *notification_s = NULL;

  notification_s = g_strdup_printf ("{ 'title': <'test notification 7'>, "
                                    "  'body': <'test notification body 7'>, "
                                    "  'icon': <%s>, "
                                    "  'default-action': <'test-action'> "
                                    "}",
                                    serialized_icon);

  if (expected_serialized_icon)
    expected_notification_s = g_strdup_printf ("{ 'title': <'test notification 7'>, "
                                               "  'body': <'test notification body 7'>, "
                                               "  'icon': <%s>, "
                                               "  'default-action': <'test-action'> "
                                               "}",
                                               expected_serialized_icon);

  run_notification_test ("test-icon", notification_s, expected_notification_s, expect_failure);
}

static void
test_themed_icon (void)
{
  g_autoptr(GVariant) serialized_icon = NULL;
  g_autoptr(GIcon) icon = NULL;
  g_autofree char *serialized_icon_s = NULL;

  icon = g_themed_icon_new ("test-icon-symbolic");
  serialized_icon = g_icon_serialize (icon);

  serialized_icon_s = g_variant_print (serialized_icon, TRUE);
  test_icon (serialized_icon_s, NULL, FALSE);
}

static void
test_bytes_icon (void)
{
  g_autoptr(GVariant) serialized_icon = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GIcon) icon = NULL;
  g_autofree char *serialized_icon_s = NULL;

  bytes = g_bytes_new_static (SVG_IMAGE_DATA, strlen (SVG_IMAGE_DATA));
  icon = g_bytes_icon_new (bytes);
  serialized_icon = g_icon_serialize (icon);

  serialized_icon_s = g_variant_print (serialized_icon, TRUE);
  test_icon (serialized_icon_s, NULL, FALSE);
}

void
test_notification_icon (void)
{
  /* For historical reasons we also accept just an icon name but it's
   * converted to a "normal" themed icon */
  test_icon ("'test-icon'", "('themed', <['test-icon', 'test-icon-symbolic']>)", FALSE);

  test_themed_icon ();
  test_bytes_icon ();

  /* Tests that should fail */
  test_icon ("('themed', <'test-icon-symbolic'>)", NULL, TRUE);
  test_icon ("('bytes', <['test-icon-symbolic', 'test-icon']>)", NULL, TRUE);
}
