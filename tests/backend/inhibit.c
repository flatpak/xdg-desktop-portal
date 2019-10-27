#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"

#include "request.h"
#include "inhibit.h"

typedef struct {
  XdpImplInhibit *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  guint flags;
  guint close_id;
  int timeout;
} InhibitHandle;

static void
inhibit_handle_free (InhibitHandle *handle)
{
  g_object_unref (handle->impl);
  if (handle->request)
    g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);

  if (handle->timeout)
    g_source_remove (handle->timeout);

  g_free (handle);
}

static gboolean
handle_close (Request *object,
              GDBusMethodInvocation *invocation,
              gpointer data)
{
  InhibitHandle *handle = g_object_get_data (G_OBJECT (object), "handle");

  if (object->exported)
    request_unexport (object);

  xdp_impl_request_complete_close (XDP_IMPL_REQUEST (object), invocation);

  g_debug ("Handling Close");

  if (handle)
    inhibit_handle_free (handle);
  else
    g_object_unref (object);

  return TRUE;
}

static gboolean
send_response (gpointer data)
{
  InhibitHandle *handle = data;
  int response;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  if (response == 0)
    {
      xdp_impl_inhibit_complete_inhibit (handle->impl, handle->invocation);
      g_object_set_data (G_OBJECT (handle->request), "handle", NULL);
      handle->request = NULL;
    }
  else
    g_dbus_method_invocation_return_error (handle->invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "Canceled");

  handle->timeout = 0;

  inhibit_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_inhibit (XdpImplInhibit *object,
                GDBusMethodInvocation *invocation,
                const char *arg_handle,
                const char *arg_app_id,
                const char *arg_parent_window,
                guint arg_flags,
                GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  InhibitHandle *handle;
  g_autoptr(Request) request = NULL;
  int delay;

  g_debug ("Handling Inhibit");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "inhibit", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (arg_flags, ==, g_key_file_get_integer (keyfile, "inhibit", "flags", NULL));

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (InhibitHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->flags = arg_flags;

  g_object_set_data (G_OBJECT (request), "handle", handle);
  handle->close_id = g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), NULL);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));


  if (g_key_file_has_key (keyfile, "backend", "delay", NULL))
    delay = g_key_file_get_integer (keyfile, "backend", "delay", NULL);
  else
    delay = 200;

  g_debug ("delay %d", delay);

  if (delay == 0)
    send_response (handle);
  else
    handle->timeout = g_timeout_add (delay, send_response, handle);

  return TRUE;
}

void
inhibit_init (GDBusConnection *connection,
              const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_inhibit_skeleton_new ());

  g_signal_connect (helper, "handle-inhibit", G_CALLBACK (handle_inhibit), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
