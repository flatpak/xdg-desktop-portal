
#include <config.h>

#include "account.h"

#include <libportal/portal.h>
#include "xdp-dbus.h"
#include "xdp-utils.h"

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"

extern char outdir[];

static int got_info;

static void
notification_action_invoked (XdpPortal *portal,
                             const char *id,
                             const char *action,
                             GVariant *platform_data,
                             GVariant *parameter,
                             gpointer data)
{
  GKeyFile *keyfile = data;
  g_autoptr(GVariant) exp_platform_data = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *exp_id = NULL;
  g_autofree char *exp_action = NULL;
  g_autofree char *platform_data_s = NULL;

  exp_id = g_key_file_get_string (keyfile, "notification", "id", NULL);
  exp_action = g_key_file_get_string (keyfile, "notification", "action", NULL);
  platform_data_s = g_key_file_get_string (keyfile, "notification", "platform_data", NULL);

  if (platform_data_s)
    {
      exp_platform_data = g_variant_parse (G_VARIANT_TYPE_VARDICT,
                                           platform_data_s, NULL, NULL, &error);
      g_assert_no_error (error);
      g_assert_true (g_variant_equal (platform_data, exp_platform_data));
    }
  else if (g_key_file_get_integer (keyfile, "notification", "force-version", NULL) == 1)
    {
      g_assert_null (platform_data);
    }
  else
    {
      g_assert_true (g_variant_is_of_type (platform_data, G_VARIANT_TYPE_VARDICT));
      g_assert_cmpuint (g_variant_n_children (platform_data), ==, 0);
    }

  g_assert_cmpstr (exp_id, ==, id);
  g_assert_cmpstr (exp_action, ==, action);

  got_info++;

  g_main_context_wakeup (NULL);
}

static void
notification_action_invoked_v1 (XdpDbusNotification *proxy,
                                const char *id,
                                const char *action,
                                GVariant *parameter,
                                gpointer data)
{
  notification_action_invoked (NULL, id, action, NULL, parameter, data);
}

void
test_notification_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  const char *notification_s;
  gulong id;

  notification_s = "{ 'title': <'title'>, "
                   "  'body': <'test notification body'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, NULL);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", notification_s);
  g_key_file_set_string (keyfile, "notification", "id", "test");
  g_key_file_set_string (keyfile, "notification", "action", "test-action");
  g_key_file_set_integer (keyfile, "backend", "delay", 200);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  id = g_signal_connect (portal, "notification-action-invoked", G_CALLBACK (notification_action_invoked), keyfile);

  got_info = 0;
  xdp_portal_add_notification (portal, "test", notification, 0, NULL, NULL, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (portal, id);

  xdp_portal_remove_notification (portal, "test");
}

void
test_notification_action_invocation_v1 (void)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(XdpDbusNotificationProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  const char *notification_s;
  gulong id;

  notification_s = "{ 'title': <'title'>, "
                   "  'body': <'test notification body'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, NULL);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", notification_s);
  g_key_file_set_string (keyfile, "notification", "id", "test-v1");
  g_key_file_set_string (keyfile, "notification", "action", "test-action");
  g_key_file_set_integer (keyfile, "notification", "force-version", 1);
  g_key_file_set_integer (keyfile, "backend", "delay", 200);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  proxy = XDP_DBUS_NOTIFICATION_PROXY(
    xdp_dbus_notification_proxy_new_sync (session_bus,
                                          0,
                                          PORTAL_BUS_NAME,
                                          PORTAL_OBJECT_PATH,
                                          NULL,
                                          &error));
  g_assert_no_error (error);

  id = g_signal_connect (XDP_DBUS_NOTIFICATION (proxy), "action-invoked",
                         G_CALLBACK (notification_action_invoked_v1), keyfile);

  got_info = 0;
  xdp_dbus_notification_call_add_notification (XDP_DBUS_NOTIFICATION (proxy),
                                              "test-v1", notification, NULL, NULL, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (proxy, id);

  xdp_dbus_notification_call_remove_notification_sync (XDP_DBUS_NOTIFICATION (proxy),
                                                       "test-v1", NULL, &error);
  g_assert_no_error (error);
}

void
test_notification_buttons (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  const char *notification_s;
  const char *platform_data_s;
  gulong id;

  notification_s = "{ 'title': <'test notification 2'>, "
                   "  'body': <'test notification body 2'>, "
                   "  'priority': <'low'>, "
                   "  'default-action': <'test-action'>, "
                   "  'buttons': <[{'label': <'button1'>, 'action': <'action1'>}, "
                   "               {'label': <'button2'>, 'action': <'action2'>}]> "
                   "}";

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, &error);
  g_assert_no_error (error);

  keyfile = g_key_file_new ();

  platform_data_s = "{ 'activation_token': <'token-123'> }";
  g_variant_unref (g_variant_parse (G_VARIANT_TYPE_VARDICT, platform_data_s, NULL, NULL, &error));
  g_assert_no_error (error);

  g_key_file_set_string (keyfile, "notification", "platform_data", platform_data_s);
  g_key_file_set_string (keyfile, "notification", "data", notification_s);
  g_key_file_set_string (keyfile, "notification", "id", "test2");
  g_key_file_set_string (keyfile, "notification", "action", "action1");
  g_key_file_set_integer (keyfile, "backend", "delay", 200);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  id = g_signal_connect (portal, "notification-action-invoked", G_CALLBACK (notification_action_invoked), keyfile);

  got_info = 0;
  xdp_portal_add_notification (portal, "test2", notification, 0, NULL, NULL, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (portal, id);

  xdp_portal_remove_notification (portal, "test2");
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
                
void
test_notification_bad_arg (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 3'>, "
                   "  'bodx': <'test notification body 3'> "
                   "}";

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, &error);
  g_assert_no_error (error);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", notification_s);
  g_key_file_set_string (keyfile, "notification", "id", "test2");
  g_key_file_set_string (keyfile, "notification", "action", "action1");
  g_key_file_set_boolean (keyfile, "backend", "expect-no-call", TRUE);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_add_notification (portal, "test3", notification, 0, NULL, notification_fail, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_notification_bad_priority (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 2'>, "
                   "  'body': <'test notification body 2'>, "
                   "  'priority': <'invalid'> "
                   "}";

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, &error);
  g_assert_no_error (error);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", notification_s);
  g_key_file_set_string (keyfile, "notification", "id", "test2");
  g_key_file_set_string (keyfile, "notification", "action", "action1");
  g_key_file_set_boolean (keyfile, "backend", "expect-no-call", TRUE);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_add_notification (portal, "test4", notification, 0, NULL, notification_fail, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_notification_bad_button (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) notification = NULL;
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'buttons': <[{'labex': <'button1'>, 'action': <'action1'>}, "
                   "               {'label': <'button2'>, 'action': <'action2'>}]> "
                   "}";

  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, &error);
  g_assert_no_error (error);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "notification", "data", notification_s);
  g_key_file_set_string (keyfile, "notification", "id", "test2");
  g_key_file_set_string (keyfile, "notification", "action", "action1");
  g_key_file_set_boolean (keyfile, "backend", "expect-no-call", TRUE);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_add_notification (portal, "test5", notification, 0, NULL, notification_fail, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}
