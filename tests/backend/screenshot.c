#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "request.h"
#include "screenshot.h"

typedef struct {
  XdpDbusImplScreenshot *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  guint timeout;
  gboolean is_screenshot;
} ScreenshotHandle;

static void
screenshot_handle_free (ScreenshotHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  if (handle->timeout)
    g_source_remove (handle->timeout);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  ScreenshotHandle *handle = data;
  GVariantBuilder opt_builder;
  g_autoptr(GVariant) params = NULL;
  int response;
  g_autofree char *uri = NULL;
  double red, green, blue;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (response == 0)
    {
      if (handle->is_screenshot)
        {
          uri = g_key_file_get_string (handle->keyfile, "result", "uri", NULL);
          g_variant_builder_add (&opt_builder, "{sv}", "uri", g_variant_new_string (uri));
        }
      else
        {
          red = g_key_file_get_double (handle->keyfile,"result", "red", NULL);
          green = g_key_file_get_double (handle->keyfile,"result", "green", NULL);
          blue = g_key_file_get_double (handle->keyfile,"result", "blue", NULL);
          g_variant_builder_add (&opt_builder, "{sv}", "color", g_variant_new ("(ddd)", red, green, blue));
        }
    }

  if (handle->request->exported)
    request_unexport (handle->request);

  g_debug ("send response %d", response);

  params = g_variant_ref_sink (g_variant_builder_end (&opt_builder));
  if (handle->is_screenshot)
    xdp_dbus_impl_screenshot_complete_screenshot (handle->impl,
                                                  handle->invocation,
                                                  response,
                                                  params);
  else
    xdp_dbus_impl_screenshot_complete_pick_color (handle->impl,
                                                  handle->invocation,
                                                  response,
                                                  params);

  handle->timeout = 0;

  screenshot_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpDbusImplRequest *object,
              GDBusMethodInvocation *invocation,
              ScreenshotHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_debug ("handling Close");
  if (handle->is_screenshot)
    xdp_dbus_impl_screenshot_complete_screenshot (handle->impl,
                                                  handle->invocation,
                                                  2,
                                                  g_variant_builder_end (&opt_builder));
  else
    xdp_dbus_impl_screenshot_complete_pick_color (handle->impl,
                                                  handle->invocation,
                                                  2,
                                                  g_variant_builder_end (&opt_builder));

  screenshot_handle_free (handle);

  return FALSE;
}


static gboolean
handle_screenshot (XdpDbusImplScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_handle,
                   const char *arg_app_id,
                   const char *arg_parent_window,
                   GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  ScreenshotHandle *handle;
  g_autoptr(Request) request = NULL;

  g_debug ("Handling %s", g_dbus_method_invocation_get_method_name (invocation));

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "screenshot", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (ScreenshotHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);

  if (strcmp (g_dbus_method_invocation_get_method_name (invocation),
              "Screenshot") == 0)
    handle->is_screenshot = TRUE;

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
screenshot_init (GDBusConnection *connection,
                 const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_screenshot_skeleton_new ());

  xdp_dbus_impl_screenshot_set_version (XDP_DBUS_IMPL_SCREENSHOT (helper), 2);
  g_signal_connect (helper, "handle-screenshot", G_CALLBACK (handle_screenshot), NULL);
  g_signal_connect (helper, "handle-pick-color", G_CALLBACK (handle_screenshot), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
