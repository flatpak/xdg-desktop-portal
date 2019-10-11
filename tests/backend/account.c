#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"

#include "account.h"

#define BACKEND_BUS_NAME "org.freedesktop.impl.portal.Test"
#define BACKEND_OBJECT_PATH "/org/freedesktop/portal/desktop"

static gboolean
handle_get_user_information (XdpImplAccount *object,
                             GDBusMethodInvocation *invocation,
                             const char *arg_handle,
                             const char *arg_app_id,
                             const char *arg_parent_window,
                             GVariant *arg_options)
{
  GVariantBuilder opt_builder;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *reason = NULL;
  g_autofree char *id = NULL;
  g_autofree char *name = NULL;
  g_autofree char *image = NULL;
  const char *r;

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "account", NULL);

  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  reason = g_key_file_get_string (keyfile, "account", "reason", &error);
  id = g_key_file_get_string (keyfile, "account", "id", &error);
  name = g_key_file_get_string (keyfile, "account", "name", &error);
  image = g_key_file_get_string (keyfile, "account", "image", &error);

  g_variant_lookup (arg_options, "reason", "&s", &r);
  if (g_strcmp0 (r, reason) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "Bad reason");
      return TRUE;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  if (id)
    g_variant_builder_add (&opt_builder, "{sv}", "id", g_variant_new_string (id));
  if (name)
    g_variant_builder_add (&opt_builder, "{sv}", "name", g_variant_new_string (name));
  if (image)
    g_variant_builder_add (&opt_builder, "{sv}", "image", g_variant_new_string (image));

  xdp_impl_account_complete_get_user_information (object,
                                                  invocation,
                                                  0,
                                                  g_variant_builder_end (&opt_builder));

  return TRUE;
}

void
account_init (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_account_skeleton_new ());

  g_signal_connect (helper, "handle-get-user-information", G_CALLBACK (handle_get_user_information), NULL);

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

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);
}
