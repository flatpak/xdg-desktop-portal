#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "request.h"
#include "account.h"

typedef struct {
  XdpDbusImplAccount *impl;
  GDBusMethodInvocation *invocation;
  XdpRequest *request;
  GKeyFile *keyfile;
  char *app_id;
  char *reason;
  guint timeout;
} AccountDialogHandle;

static void
account_dialog_handle_free (AccountDialogHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  g_free (handle->reason);
  if (handle->timeout)
    g_source_remove (handle->timeout);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  AccountDialogHandle *handle = data;
  GVariantBuilder opt_builder;
  g_autofree char *reason = NULL;
  g_autofree char *id = NULL;
  g_autofree char *name = NULL;
  g_autofree char *image = NULL;
  g_autoptr(GError) error = NULL;
  int response;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  reason = g_key_file_get_string (handle->keyfile, "backend", "reason", &error);
  id = g_key_file_get_string (handle->keyfile, "account", "id", NULL);
  name = g_key_file_get_string (handle->keyfile, "account", "name", NULL);
  image = g_key_file_get_string (handle->keyfile, "account", "image", NULL);

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  if (g_strcmp0 (handle->reason, reason) != 0)
    {
      g_dbus_method_invocation_return_error (handle->invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "Unexpected reason: '%s' != '%s'", reason, handle->reason);
      return TRUE;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  if (id)
    g_variant_builder_add (&opt_builder, "{sv}", "id", g_variant_new_string (id));
  if (name)
    g_variant_builder_add (&opt_builder, "{sv}", "name", g_variant_new_string (name));
  if (image)
    g_variant_builder_add (&opt_builder, "{sv}", "image", g_variant_new_string (image));

  if (handle->request->exported)
    xdp_request_unexport (handle->request);

  g_debug ("send response %d", response);

  xdp_dbus_impl_account_complete_get_user_information (handle->impl,
                                                       handle->invocation,
                                                       response,
                                                       g_variant_builder_end (&opt_builder));

  handle->timeout = 0;

  account_dialog_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpDbusImplRequest *object,
              GDBusMethodInvocation *invocation,
              AccountDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_debug ("send response 2");
  xdp_dbus_impl_account_complete_get_user_information (handle->impl,
                                                       handle->invocation,
                                                       2,
                                                       g_variant_builder_end (&opt_builder));
  account_dialog_handle_free (handle);

  return FALSE;
}


static gboolean
handle_get_user_information (XdpDbusImplAccount *object,
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
  AccountDialogHandle *handle;
  const char *reason = NULL;
  g_autoptr(XdpRequest) request = NULL;

  g_debug ("Handling GetUserInformation");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "account", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = xdp_request_new (sender, arg_app_id, arg_handle);

  g_variant_lookup (arg_options, "reason", "&s", &reason);

  handle = g_new0 (AccountDialogHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->reason = g_strdup (reason);

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
account_init (GDBusConnection *connection,
              const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_account_skeleton_new ());

  g_signal_connect (helper, "handle-get-user-information", G_CALLBACK (handle_get_user_information), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
