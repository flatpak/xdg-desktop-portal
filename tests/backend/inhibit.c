#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"

#include "request.h"
#include "inhibit.h"

typedef struct {
  XdpImplInhibit *impl;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  guint flags;
} InhibitHandle;

static void
inhibit_handle_free (InhibitHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);

  g_free (handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              InhibitHandle *handle)
{
  if (handle->request->exported)
    request_unexport (handle->request);

  inhibit_handle_free (handle);

  return FALSE;
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
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->flags = arg_flags;

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_impl_inhibit_complete_inhibit (handle->impl, invocation);

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
