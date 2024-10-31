
#include <config.h>

#include "account.h"

#include <libportal/portal.h>
#include "xdp-utils.h"

#define SVG_IMAGE_DATA \
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
  "<svg xmlns=\"http://www.w3.org/2000/svg\" height=\"16px\" width=\"16px\"/>"


/* reenable when the proper PR in libportal is merged */
#if 0
static const guchar SOUND_DATA[] = {
  0x52, 0x49, 0x46, 0x46, 0x24, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45,
  0x66, 0x6d, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
  0x44, 0xac, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
  0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00
};
#endif

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
notification_fail_no_error_check (GObject *source,
                                  GAsyncResult *result,
                                  gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (source);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_add_notification_finish (portal, result, &error);
  g_assert_false (res);

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
run_notification_test_with_callback (const char          *notification_id,
                                     const char          *notification_s,
                                     const char          *expected_notification_s,
                                     gboolean             expect_failure,
                                     GAsyncReadyCallback  callback)
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

  if (!callback)
    callback = (expect_failure) ? notification_fail : notification_succeed;

  xdp_portal_add_notification (portal, notification_id, notification, 0, NULL, callback, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  if (!expect_failure)
    g_signal_handler_disconnect (portal, id);

  xdp_portal_remove_notification (portal, notification_id);
}

static void
run_notification_test (const char *notification_id,
                       const char *notification_s,
                       const char *expected_notification_s,
                       gboolean    expect_failure)
{
    run_notification_test_with_callback (notification_id, notification_s, expected_notification_s, expect_failure, NULL);
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
test_notification_markup_body (void)
{
  const char *notification_s;
  const char *exp_notification_s;

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test <b>notification</b> body <i>italic</i>'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test ("test3", notification_s, NULL, FALSE);

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test <a href=\"https://example.com\"><b>Some link</b></a>'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test ("test3", notification_s, NULL, FALSE);

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test \n newline \n\n some more space \n  with trailing space '>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  exp_notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test newline some more space with trailing space'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test ("test3", notification_s, exp_notification_s, FALSE);

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test \n newline \n\n some more space \n  with trailing space '>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  exp_notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test newline some more space with trailing space'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test ("test3", notification_s, exp_notification_s, FALSE);

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test <custom> tag </custom>'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  exp_notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test tag'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test ("test3", notification_s, exp_notification_s, FALSE);

  /* Tests that should fail */
  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test <b>notification<b> body'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test_with_callback ("test3", notification_s, NULL, TRUE, notification_fail_no_error_check);

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'<b>foo<i>bar</b></i>'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test_with_callback ("test3", notification_s, NULL, TRUE, notification_fail_no_error_check);

  notification_s = "{ 'title': <'title'>, "
                   "  'markup-body': <'test <markup><i>notification</i><markup> body'>, "
                   "  'priority': <'normal'>, "
                   "  'default-action': <'test-action'> }";

  run_notification_test_with_callback ("test3", notification_s, NULL, TRUE, notification_fail_no_error_check);
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
  test_icon (serialized_icon_s, "('file-descriptor', <handle 0>)", FALSE);
}


/* reenable when the proper PR in libportal is merged */
#if 0
static void
test_file_icon (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileIOStream) iostream = NULL;
  g_autoptr(GVariant) serialized_icon = NULL;
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GFile) file = NULL;
  GOutputStream *stream = NULL;
  g_autofree char *exp_serialized_icon_s = NULL;
  g_autofree char *serialized_icon_s = NULL;
  g_autofree char *uri = NULL;

  file = g_file_new_tmp ("iconXXXXXX", &iostream, NULL);
  stream = g_io_stream_get_output_stream (G_IO_STREAM (iostream));
  g_output_stream_write_all (stream, SVG_IMAGE_DATA, strlen (SVG_IMAGE_DATA), NULL, NULL, NULL);
  g_output_stream_close (stream, NULL, NULL);

  uri = g_file_get_uri (file);
  icon = g_file_icon_new (file);
  serialized_icon = g_icon_serialize (icon);
  serialized_icon_s = g_variant_print (serialized_icon, TRUE);
  test_icon (serialized_icon_s, "('file-descriptor', <handle 0>)", FALSE);
  g_file_delete (file, NULL, &error);
  g_assert_no_error (error);
}
#endif

void
test_notification_icon (void)
{
  /* For historical reasons we also accept just an icon name but it's
   * converted to a "normal" themed icon */
  test_icon ("'test-icon'", "('themed', <['test-icon', 'test-icon-symbolic']>)", FALSE);

  test_themed_icon ();
  test_bytes_icon ();

/* reenable when the proper PR in libportal is merged */
#if 0
  test_file_icon ();
#endif

  /* Tests that should fail */
  test_icon ("('themed', <'test-icon-symbolic'>)", NULL, TRUE);
  test_icon ("('bytes', <['test-icon-symbolic', 'test-icon']>)", NULL, TRUE);
  test_icon ("('file-descriptor', <''>)", NULL, TRUE);
  test_icon ("('file-descriptor', <handle 0>)", NULL, TRUE);
}

static void
test_sound (const char *serialized_sound,
            const char *expected_serialized_sound,
            gboolean    expect_failure)
{
  g_autofree char *notification_s = NULL;
  g_autofree char *expected_notification_s = NULL;

  notification_s = g_strdup_printf ("{ 'title': <'test notification 7'>, "
                                    "  'body': <'test notification body 7'>, "
                                    "  'sound': <%s>, "
                                    "  'default-action': <'test-action'> "
                                    "}", serialized_sound);

  if (expected_serialized_sound)
    expected_notification_s = g_strdup_printf ("{ 'title': <'test notification 7'>, "
                                               "  'body': <'test notification body 7'>, "
                                               "  'sound': <%s>, "
                                               "  'default-action': <'test-action'> "
                                               "}", expected_serialized_sound);

  run_notification_test ("test-sound", notification_s, expected_notification_s, expect_failure);
}

/* reenable when the proper PR in libportal is merged */
#if 0
static void
test_file_sound (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *serialized_sound_s = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileIOStream) iostream = NULL;
  GOutputStream *stream = NULL;

  file = g_file_new_tmp ("soundXXXXXX", &iostream, NULL);
  stream = g_io_stream_get_output_stream (G_IO_STREAM (iostream));
  g_output_stream_write_all (stream, SOUND_DATA, sizeof (SOUND_DATA), NULL, NULL, NULL);
  g_output_stream_close (stream, NULL, NULL);

  uri = g_file_get_uri (file);
  serialized_sound_s = g_strdup_printf ("('file', <'%s'>)", uri);
  test_sound (serialized_sound_s, "('file-descriptor', <handle 0>)", FALSE);
  g_file_delete (file, NULL, &error);
  g_assert_no_error (error);
}
#endif

void
test_notification_sound (void)
{
  test_sound ("'default'", NULL, FALSE);
  test_sound ("'silent'", NULL, FALSE);
/* reenable when the proper PR in libportal is merged */
#if 0
  test_file_sound ();
#endif

  /* Tests that should fail */
  test_sound ("('file-descriptor', <''>)", NULL, TRUE);
  test_sound ("('file-descriptor', <handle 0>)", NULL, TRUE);
}

void
test_notification_display_hint (void)
{
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'display-hint': <['transient', 'show-as-new']>"
                   "}";

  run_notification_test ("test5", notification_s, NULL, FALSE);

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'display-hint': <['unsupported-hint']>"
                   "}";

  run_notification_test ("test5", notification_s, NULL, TRUE);
}

void
test_notification_category (void)
{
  const char *notification_s;

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'category': <'im.received'>"
                   "}";

  run_notification_test ("test5", notification_s, NULL, FALSE);

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'category': <'x-vendor.custom'>"
                   "}";

  run_notification_test ("test5", notification_s, NULL, FALSE);

  notification_s = "{ 'title': <'test notification 5'>, "
                   "  'body': <'test notification body 5'>, "
                   "  'category': <'unsupported-type'>"
                   "}";

  run_notification_test ("test5", notification_s, NULL, TRUE);
}

/* reenable when the proper PR in libportal is merged */
#if 0
void
test_notification_supported_properties (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  const char *expected_serialized;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GVariant) expected_response = NULL;
  g_autoptr(GVariant) response = NULL;

  keyfile = g_key_file_new ();

  expected_serialized = "{ 'something': <'sdfs'> }";
  g_key_file_set_string (keyfile, "notification", "supported-options", expected_serialized);

  path = g_build_filename (outdir, "notification", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  /* Wait for the backend to update the supported options */
  sleep (1);

  portal = xdp_portal_new ();

  response = xdp_portal_get_supported_notification_options (portal, &error);
  g_assert_no_error (error);
  expected_response = g_variant_parse (G_VARIANT_TYPE_VARDICT, expected_serialized, NULL, NULL, NULL);
  g_assert_true (g_variant_equal (expected_response, response));
}
#endif
