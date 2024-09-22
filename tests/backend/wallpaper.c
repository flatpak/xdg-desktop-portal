#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "request.h"
#include "wallpaper.h"

typedef struct {
  XdpDbusImplWallpaper *impl;
  GDBusMethodInvocation *invocation;
  XdpRequest *request;
  GKeyFile *keyfile;
  char *app_id;
  guint timeout;
  char *uri;
  GVariant *options;
} WallpaperHandle;

static void
wallpaper_handle_free (WallpaperHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  if (handle->timeout)
    g_source_remove (handle->timeout);
  g_free (handle->uri);
  g_variant_unref (handle->options);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  WallpaperHandle *handle = data;
  int response;
  g_autofree char *s1 = NULL;
  const char *s;
  gboolean b1, b;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  if (handle->request->exported)
    xdp_request_unexport (handle->request);

  s1 = g_key_file_get_string (handle->keyfile, "wallpaper", "target", NULL);
  g_variant_lookup (handle->options, "set-on", "&s", &s);
  g_assert_cmpstr (s1, ==, s);

  b1 = g_key_file_get_boolean (handle->keyfile, "wallpaper", "preview", NULL);
  g_variant_lookup (handle->options, "show-preview", "b", &b);
  g_assert_cmpint (b1, ==, b);

  g_debug ("send response %d", response);

  xdp_dbus_impl_wallpaper_complete_set_wallpaper_uri (handle->impl,
                                                      handle->invocation,
                                                      response);

  handle->timeout = 0;

  wallpaper_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpDbusImplRequest *object,
              GDBusMethodInvocation *invocation,
              WallpaperHandle *handle)
{

  g_debug ("send response 2");
  xdp_dbus_impl_wallpaper_complete_set_wallpaper_uri (handle->impl,
                                                      handle->invocation,
                                                      2);
  wallpaper_handle_free (handle);

  return FALSE;
}


static gboolean
handle_set_wallpaper_uri (XdpDbusImplWallpaper *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_handle,
                          const char *arg_app_id,
                          const char *arg_parent_window,
                          const char *arg_uri,
                          GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  WallpaperHandle *handle;
  g_autoptr(XdpRequest) request = NULL;

  g_debug ("Handling SetWallpaperURI");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "wallpaper", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = xdp_request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (WallpaperHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->uri = g_strdup (arg_uri);
  handle->options = g_variant_ref (arg_options);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

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
wallpaper_init (GDBusConnection *connection,
                const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_wallpaper_skeleton_new ());

  g_signal_connect (helper, "handle-set-wallpaper-uri", G_CALLBACK (handle_set_wallpaper_uri), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
