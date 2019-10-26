#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"

#include "request.h"
#include "print.h"

typedef struct {
  XdpImplPrint *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  guint timeout;
  char *title;
  GVariant *settings;
  GVariant *page_setup;
  GVariant *options;
} PrintHandle;

static void
print_handle_free (PrintHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  if (handle->timeout)
    g_source_remove (handle->timeout);
  g_free (handle->title);
  if (handle->settings)
    g_variant_unref (handle->settings);
  if (handle->page_setup)
    g_variant_unref (handle->page_setup);
  if (handle->options)
    g_variant_unref (handle->options);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  PrintHandle *handle = data;
  GVariantBuilder opt_builder;
  int response;
  int token;
  GVariantBuilder settings;
  GVariantBuilder page_setup;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    request_unexport (handle->request);

  if (strcmp (g_dbus_method_invocation_get_method_name (handle->invocation), "PreparePrint") == 0)
    {
      token = g_key_file_get_integer (handle->keyfile, "result", "token", NULL);
      g_variant_builder_init (&settings, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_init (&page_setup, G_VARIANT_TYPE_VARDICT);

      g_variant_builder_add (&opt_builder, "{sv}", "token", g_variant_new_uint32 (token));
      g_variant_builder_add (&opt_builder, "{sv}", "settings", g_variant_builder_end (&settings));
      g_variant_builder_add (&opt_builder, "{sv}", "page-setup", g_variant_builder_end (&page_setup));
    }

  g_debug ("send response %d", response);
 
  if (strcmp (g_dbus_method_invocation_get_method_name (handle->invocation), "Print") == 0)
    xdp_impl_print_complete_print (handle->impl,
                                   handle->invocation,
                                   NULL,
                                   response,
                                   g_variant_builder_end (&opt_builder));
  else
    xdp_impl_print_complete_prepare_print (handle->impl,
                                           handle->invocation,
                                           response,
                                           g_variant_builder_end (&opt_builder));

  handle->timeout = 0;

  print_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              PrintHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_debug ("send response 2");
  if (strcmp (g_dbus_method_invocation_get_method_name (handle->invocation), "Print") == 0)
    xdp_impl_print_complete_print (handle->impl,
                                   handle->invocation,
                                   NULL,
                                   2,
                                   g_variant_builder_end (&opt_builder));
  else
    xdp_impl_print_complete_prepare_print (handle->impl,
                                           handle->invocation,
                                           2,
                                           g_variant_builder_end (&opt_builder));
  print_handle_free (handle);

  return FALSE;
}


static gboolean
handle_print (XdpImplPrint *object,
              GDBusMethodInvocation *invocation,
	      GUnixFDList *fd_list,
              const char *arg_handle,
              const char *arg_app_id,
              const char *arg_parent_window,
              const char *arg_title,
	      GVariant *arg_fd,
              GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  PrintHandle *handle;
  g_autoptr(Request) request = NULL;

  g_debug ("Handling Print");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "print", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (PrintHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);

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

static gboolean
handle_prepare_print (XdpImplPrint *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_handle,
                      const char *arg_app_id,
                      const char *arg_parent_window,
                      const char *arg_title,
		      GVariant *arg_settings,
		      GVariant *arg_page_setup,
                      GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  int delay;
  PrintHandle *handle;
  g_autoptr(Request) request = NULL;

  g_debug ("Handling Print");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "print", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (PrintHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);

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
print_init (GDBusConnection *connection,
            const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_print_skeleton_new ());

  g_signal_connect (helper, "handle-print", G_CALLBACK (handle_print), NULL);
  g_signal_connect (helper, "handle-prepare-print", G_CALLBACK (handle_prepare_print), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
