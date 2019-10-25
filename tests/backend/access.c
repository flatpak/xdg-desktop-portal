#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"

#include "request.h"
#include "access.h"

typedef struct {
  XdpImplAccess *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  char *title;
  char *subtitle;
  char *body;
  GVariant *options;
  guint timeout;
} AccessHandle;

static void
access_handle_free (AccessHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  g_free (handle->title);
  g_free (handle->subtitle);
  g_free (handle->body);
  if (handle->timeout)
    g_source_remove (handle->timeout);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  AccessHandle *handle = data;
  GVariantBuilder opt_builder;
  int response;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    request_unexport (handle->request);

  g_debug ("send response %d", response);

  xdp_impl_access_complete_access_dialog (handle->impl,
                                          handle->invocation,
                                          response,
                                          g_variant_builder_end (&opt_builder));

  handle->timeout = 0;

  access_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              AccessHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_debug ("AccessDialog handling Close");
  xdp_impl_access_complete_access_dialog (handle->impl,
                                          handle->invocation,
                                          2,
                                          g_variant_builder_end (&opt_builder));
  access_handle_free (handle);

  return FALSE;
}

static gboolean
handle_access_dialog (XdpImplAccess *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_handle,
                      const char *arg_app_id,
                      const char *arg_parent_window,
                      const char *arg_title,
                      const char *arg_subtitle,
                      const char *arg_body,
                      GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  AccessHandle *handle;
  g_autoptr(Request) request = NULL;

  g_debug ("Handling AccessDialog");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "access", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (AccessHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->title = g_strdup (arg_title);
  handle->subtitle = g_strdup (arg_subtitle);
  handle->body = g_strdup (arg_body);

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
access_init (GDBusConnection *connection,
             const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_access_skeleton_new ());

  g_signal_connect (helper, "handle-access-dialog", G_CALLBACK (handle_access_dialog), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
