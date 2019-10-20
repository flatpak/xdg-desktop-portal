#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"

#include "request.h"
#include "account.h"

#define BACKEND_OBJECT_PATH "/org/freedesktop/portal/desktop"

typedef struct {
  XdpImplFileChooser *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  char *title;
  GVariant *options;
  guint timeout;
} FileChooserHandle;

static void
file_chooser_handle_free (FileChooserHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  if (handle->timeout)
    g_source_remove (handle->timeout);
  g_free (handle->title);
  g_variant_unref (handle->options);
  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  FileChooserHandle *handle = data;
  GVariantBuilder opt_builder;
  g_autofree char *reason = NULL;
  g_autofree char *id = NULL;
  g_autofree char *name = NULL;
  g_autofree char *image = NULL;
  g_autoptr(GError) error = NULL;
  int response;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (response == 0)
    {
      g_auto(GStrv) uris = NULL;

      uris = g_key_file_get_string_list (handle->keyfile, "result", "uris", NULL, NULL);
      g_variant_builder_add (&opt_builder, "{sv}", "uris", g_variant_new_strv ((const char * const *)uris, -1));
    }

  if (handle->request->exported)
    request_unexport (handle->request);

  g_debug ("send response %d", response);

  xdp_impl_file_chooser_complete_open_file (handle->impl,
                                            handle->invocation,
                                            response,
                                            g_variant_builder_end (&opt_builder));

  handle->timeout = 0;

  file_chooser_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              FileChooserHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_debug ("send response 2");
  xdp_impl_file_chooser_complete_open_file (handle->impl,
                                            handle->invocation,
                                            2,
                                            g_variant_builder_end (&opt_builder));
  file_chooser_handle_free (handle);

  return FALSE;
}


static gboolean
handle_open_file (XdpImplFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const char *arg_handle,
                  const char *arg_app_id,
                  const char *arg_parent_window,
                  const char *arg_title,
                  GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  FileChooserHandle *handle;
  g_autoptr(Request) request = NULL;

  g_debug ("Handling OpenFile");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "filechooser", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (FileChooserHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->title = g_strdup (arg_title);
  handle->options = g_variant_ref (arg_options);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

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
file_chooser_init (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_file_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_open_file), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         connection,
                                         BACKEND_OBJECT_PATH,
                                         &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, BACKEND_OBJECT_PATH);
}
