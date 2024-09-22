#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "request.h"
#include "appchooser.h"

typedef struct {
  XdpDbusImplAppChooser *impl;
  GDBusMethodInvocation *invocation;
  XdpRequest *request;
  GKeyFile *keyfile;
  char *app_id;
  guint timeout;
  char **choices;
  GVariant *options;
} AppChooserHandle;

static void
app_chooser_handle_free (AppChooserHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  if (handle->timeout)
    g_source_remove (handle->timeout);
  g_strfreev (handle->choices);
  g_variant_unref (handle->options);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  AppChooserHandle *handle = data;
  GVariantBuilder opt_builder;
  int response;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    xdp_request_unexport (handle->request);

  if (response == 0)
    {
      if (handle->choices[0])
        {
          g_debug ("choice: %s", handle->choices[0]);
          g_variant_builder_add (&opt_builder, "{sv}", "choice", g_variant_new_string (handle->choices[0]));
        }
    }

  g_debug ("send response %d", response);

  xdp_dbus_impl_app_chooser_complete_choose_application (handle->impl,
                                                         handle->invocation,
                                                         response,
                                                         g_variant_builder_end (&opt_builder));

  handle->timeout = 0;

  app_chooser_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpDbusImplRequest *object,
              GDBusMethodInvocation *invocation,
              AppChooserHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_dbus_impl_app_chooser_complete_choose_application (handle->impl,
                                                         handle->invocation,
                                                         2,
                                                         g_variant_builder_end (&opt_builder));
  app_chooser_handle_free (handle);

  return FALSE;
}


static gboolean
handle_choose_application (XdpDbusImplAppChooser *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_handle,
                           const char *arg_app_id,
                           const char *arg_parent_window,
                           const char * const *arg_choices,
                           GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  AppChooserHandle *handle;
  g_autoptr(XdpRequest) request = NULL;

  g_debug ("Handling ChooseApplication");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "appchooser", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  if (g_key_file_has_key (keyfile, "backend", "expect-no-call", NULL))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "Did not expect ChooseApplication to be called here");
      return TRUE;  /* handled */
    }

  request = xdp_request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (AppChooserHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->choices = g_strdupv ((char **)arg_choices);
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
appchooser_init (GDBusConnection *connection,
                 const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_app_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-choose-application", G_CALLBACK (handle_choose_application), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
