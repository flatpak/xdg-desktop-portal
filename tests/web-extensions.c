#include <config.h>

#include "web-extensions.h"
#include "xdp-utils.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "xdp-impl-dbus.h"

// TODO: convert these simple client wrappers to proper libportal APIs

static void
create_session_returned (GObject *object,
                         GAsyncResult *result,
                         gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GVariant) ret = NULL;
  GError *error = NULL;
  g_autofree char *session_handle = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  if (!ret)
    {
      g_task_return_error (task, error);
      return;
    }
  g_variant_get (ret, "(o)", &session_handle);
  g_task_return_pointer (task, g_steal_pointer (&session_handle), g_free);
}

static void
create_session (GCancellable *cancellable,
                GAsyncReadyCallback callback,
                gpointer data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  GError *error = NULL;
  g_autofree char *session_token = NULL;
  GVariantBuilder options;

  task = g_task_new (NULL, cancellable, callback, data);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  session_token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "mode", g_variant_new_string ("mozilla"));
  g_variant_builder_add (&options, "{sv}", "session_handle_token", g_variant_new_string (session_token));
  g_dbus_connection_call (session_bus,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.WebExtensions",
                          "CreateSession",
                          g_variant_new ("(a{sv})", &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          create_session_returned,
                          g_steal_pointer (&task));
}

static char *
create_session_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
get_manifest_returned (GObject *object,
                       GAsyncResult *result,
                       gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GVariant) ret = NULL;
  GError *error = NULL;
  g_autofree char *json_manifest = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  if (!ret)
    {
      g_task_return_error (task, error);
      return;
    }
  g_variant_get (ret, "(s)", &json_manifest);
  g_task_return_pointer (task, g_steal_pointer (&json_manifest), g_free);
}

static void
get_manifest (const char *session_handle,
              const char *name,
              const char *extension_or_origin,
              GCancellable *cancellable,
              GAsyncReadyCallback callback,
              gpointer data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  GError *error = NULL;

  task = g_task_new (NULL, cancellable, callback, data);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  g_dbus_connection_call (session_bus,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.WebExtensions",
                          "GetManifest",
                          g_variant_new ("(oss)", session_handle, name, extension_or_origin),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          get_manifest_returned,
                          g_steal_pointer (&task));
}

static char *
get_manifest_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
start_returned (GObject *object,
                GAsyncResult *result,
                gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GVariant) ret = NULL;
  GError *error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  if (!ret)
    {
      guint signal_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (task), "response-signal-id"));
      g_dbus_connection_signal_unsubscribe (G_DBUS_CONNECTION (object), signal_id);
      g_task_return_error (task, error);
      return;
    }
}

static void
start_completed (GDBusConnection *session_bus,
                 const char *sender_name,
                 const char *object_path,
                 const char *interface_name,
                 const char *signal_name,
                 GVariant *parameters,
                 gpointer data)
{
  g_autoptr(GTask) task = g_object_ref (data);
  guint signal_id;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  signal_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (task), "response-signal-id"));
  g_dbus_connection_signal_unsubscribe (session_bus, signal_id);

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);
  switch (response)
    {
    case 0:
      g_task_return_boolean (task, TRUE);
      break;
    case 1:
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Start cancelled");
      break;
    case 2:
    default:
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Start failed");
      break;
    }
}

static void
start (const char *session_handle,
       const char *name,
       const char *extension_or_origin,
       GCancellable *cancellable,
       GAsyncReadyCallback callback,
       gpointer data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  GError *error = NULL;
  g_autofree char *token = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *request_path = NULL;
  int i;
  guint signal_id;
  GVariantBuilder options;

  task = g_task_new (NULL, cancellable, callback, data);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  sender = g_strdup (g_dbus_connection_get_unique_name (session_bus) + 1);
  for (i = 0; sender[i]; i++)
    if (sender[i] == '.')
      sender[i] = '_';
  request_path = g_strconcat ("/org/freedesktop/portal/desktop/request/", sender, "/", token, NULL);
  signal_id = g_dbus_connection_signal_subscribe (session_bus,
                                                  "org.freedesktop.portal.Desktop",
                                                  "org.freedesktop.portal.Request",
                                                  "Response",
                                                  request_path,
                                                  NULL,
                                                  G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                  start_completed,
                                                  g_object_ref (task),
                                                  g_object_unref);
  g_object_set_data (G_OBJECT (task), "response-signal-id", GUINT_TO_POINTER (signal_id));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_dbus_connection_call (session_bus,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.WebExtensions",
                          "Start",
                          g_variant_new ("(ossa{sv})", session_handle, name, extension_or_origin, &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          start_returned,
                          g_steal_pointer (&task));
}

static gboolean
start_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
get_pipes_returned (GObject *object,
                    GAsyncResult *result,
                    gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  GError *error = NULL;

  ret = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (object), &fd_list, result, &error);
  if (!ret)
    {
      g_task_return_error (task, error);
      return;
    }
  g_task_return_pointer (task, g_steal_pointer (&fd_list), g_object_unref);
}

static void
get_pipes (const char *session_handle,
           GCancellable *cancellable,
           GAsyncReadyCallback callback,
           gpointer data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  GError *error = NULL;

  task = g_task_new (NULL, cancellable, callback, data);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  g_dbus_connection_call_with_unix_fd_list (session_bus,
                                            "org.freedesktop.portal.Desktop",
                                            "/org/freedesktop/portal/desktop",
                                            "org.freedesktop.portal.WebExtensions",
                                            "GetPipes",
                                            g_variant_new ("(oa{sv})", session_handle, NULL),
                                            NULL,
                                            G_DBUS_CALL_FLAGS_NONE,
                                            -1,
                                            NULL,
                                            cancellable,
                                            get_pipes_returned,
                                            g_steal_pointer (&task));
}

static gboolean
get_pipes_finish (int *stdin_fileno, int *stdout_fileno, int *stderr_fileno, GAsyncResult *result, GError **error)
{
  g_autoptr(GUnixFDList) fd_list = NULL;

  fd_list = g_task_propagate_pointer (G_TASK (result), error);
  if (fd_list == NULL)
    return FALSE;

  if (stdin_fileno != NULL)
    {
      *stdin_fileno = g_unix_fd_list_get (fd_list, 0, error);
      if (*stdin_fileno < 0)
        return FALSE;
    }
  if (stdout_fileno != NULL)
    {
      *stdout_fileno = g_unix_fd_list_get (fd_list, 1, error);
      if (*stdout_fileno < 0)
        return FALSE;
    }
  if (stderr_fileno != NULL)
    {
      *stderr_fileno = g_unix_fd_list_get (fd_list, 2, error);
      if (*stderr_fileno < 0)
        return FALSE;
    }
  return TRUE;
}

static void
close_session_returned (GObject *object,
                        GAsyncResult *result,
                        gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GVariant) ret = NULL;
  GError *error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  if (!ret)
    {
      g_task_return_error (task, error);
      return;
    }
  g_task_return_boolean (task, TRUE);
}

static void
close_session (const char *session_handle,
               GCancellable *cancellable,
               GAsyncReadyCallback callback,
               gpointer data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  GError *error = NULL;

  task = g_task_new (NULL, cancellable, callback, data);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  g_dbus_connection_call (session_bus,
                          "org.freedesktop.portal.Desktop",
                          session_handle,
                          "org.freedesktop.portal.Session",
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          close_session_returned,
                          g_steal_pointer (&task));
}

static gboolean
close_session_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}


extern char outdir[];

static int got_info = 0;

extern XdpDbusImplPermissionStore *permission_store;

static void
set_web_extensions_permissions (const char *permission)
{
  const char *permissions[2] = { NULL, NULL };
  g_autoptr(GError) error = NULL;

  permissions[0] = permission;
  xdp_dbus_impl_permission_store_call_set_permission_sync (permission_store,
                                                           "webextensions",
                                                           TRUE,
                                                           "org.example.testing",
                                                           "",
                                                           permissions,
                                                           NULL,
                                                           &error);
  g_assert_no_error (error);
}

static gboolean
cancel_call (gpointer data)
{
  GCancellable *cancellable = data;

  g_debug ("cancel call");
  g_cancellable_cancel (cancellable);

  return G_SOURCE_REMOVE;
}

typedef struct {
  GCancellable *cancellable;
  char *session_handle;
  const char *messaging_host_name;
} TestData;

static void
close_session_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  gboolean ret;

  ret = close_session_finish (result, &error);
  if (ret)
    {
      g_assert_no_error (error);
    }
  else
    {
      /* The native messaging host may have closed before we tried to
         close it. */
      g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD);
    }

  got_info++;
  g_main_context_wakeup (NULL);
}

static void
get_pipes_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  TestData *test_data = data;
  g_autoptr(GError) error = NULL;
  gboolean ret;
  int stdin_fileno = -1, stdout_fileno = -1, stderr_fileno = -1;

  ret = get_pipes_finish (&stdin_fileno, &stdout_fileno, &stderr_fileno, result, &error);
  g_assert_no_error (error);
  g_assert_true (ret);
  g_assert_cmpint (stdin_fileno, >, 0);
  g_assert_cmpint (stdout_fileno, >, 0);
  g_assert_cmpint (stderr_fileno, >, 0);

  close (stdin_fileno);
  close (stdout_fileno);
  close (stderr_fileno);

  close_session (test_data->session_handle,
                 test_data->cancellable,
                 close_session_cb,
                 test_data);
}

static void
start_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  TestData *test_data = data;
  g_autoptr(GError) error = NULL;
  gboolean ret;

  ret = start_finish (result, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  get_pipes (test_data->session_handle,
             test_data->cancellable,
             get_pipes_cb,
             test_data);
}


static void
get_manifest_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  TestData *test_data = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *json_manifest = NULL;
  g_autofree char *host_path = NULL;
  g_autofree char *expected = NULL;

  host_path = g_test_build_filename (G_TEST_BUILT, "native-messaging-hosts", "server.sh", NULL);
  expected = g_strdup_printf ("{\"name\":\"org.example.testing\",\"description\":\"Test native messaging host\",\"path\":\"%s\",\"type\":\"stdio\",\"allowed_extensions\":[\"some-extension@example.org\"]}", host_path);

  json_manifest = get_manifest_finish (result, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (json_manifest, ==, expected);

  start (test_data->session_handle,
         "org.example.testing",
         "some-extension@example.org",
         test_data->cancellable,
         start_cb,
         test_data);
}

static void
create_session_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  TestData *test_data = data;
  g_autoptr(GError) error = NULL;

  test_data->session_handle = create_session_finish (result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (test_data->session_handle);

  get_manifest (test_data->session_handle,
                "org.example.testing",
                "some-extension@example.org",
                test_data->cancellable,
                get_manifest_cb,
                test_data);
}

void
test_web_extensions_basic (void)
{
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancellable = NULL;
  TestData test_data = { cancellable, NULL };

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  got_info = 0;

  set_web_extensions_permissions ("yes");
  create_session (cancellable, create_session_cb, &test_data);

  g_timeout_add (100, cancel_call, cancellable);
  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
  g_free (test_data.session_handle);
}

static void
start_bad_name_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  gboolean ret;

  ret = start_finish (result, &error);
  g_assert_false (ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);

  got_info++;
  g_main_context_wakeup (NULL);
}

static void
create_session_bad_name_cb (GObject *object, GAsyncResult *result, gpointer data)
{
  TestData *test_data = data;
  g_autoptr(GError) error = NULL;

  test_data->session_handle = create_session_finish (result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (test_data->session_handle);

  start (test_data->session_handle,
         test_data->messaging_host_name,
         "some-extension@example.org",
         test_data->cancellable,
         start_bad_name_cb,
         test_data);
}

void
test_web_extensions_bad_name (void)
{
    const char *messaging_host_name[] = {
        "no-dashes",
        "../foo",
        "no_trailing_dot.",
    };
    int i;

    for (i = 0; i < G_N_ELEMENTS (messaging_host_name); i++)
      {
        g_autoptr(GCancellable) cancellable = NULL;
        TestData test_data = { cancellable, NULL, messaging_host_name[i] };

        got_info = 0;
        set_web_extensions_permissions ("yes");
        create_session (cancellable, create_session_bad_name_cb, &test_data);

        g_timeout_add (100, cancel_call, cancellable);
        while (!got_info)
          g_main_context_iteration (NULL, TRUE);
        g_free (test_data.session_handle);
    }
}
