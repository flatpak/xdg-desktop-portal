#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "request.h"
#include "background.h"


static gboolean
handle_get_app_state (XdpDbusImplBackground *object,
                      GDBusMethodInvocation *invocation)
{
  GVariantBuilder builder;

  g_debug ("background: handle GetAppState");

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  xdp_dbus_impl_background_complete_get_app_state (object,
                                              invocation,
                                              g_variant_builder_end (&builder));

  return TRUE;
}

static gboolean
handle_notify_background (XdpDbusImplBackground *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_handle,
                          const char *arg_app_id,
                          const char *arg_name)
{
  GVariantBuilder opt_builder;

  g_debug ("background: handle NotifyBackground");

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_dbus_impl_background_complete_notify_background (object,
                                                       invocation,
                                                       2,
                                                       g_variant_builder_end (&opt_builder));

  return TRUE;
}

static gboolean
handle_enable_autostart (XdpDbusImplBackground *object,
                         GDBusMethodInvocation *invocation,
                         const char *arg_app_id,
                         gboolean arg_enable,
                         const char * const *arg_commandline,
                         guint arg_flags)
{
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("background: handle EnableAutostart");

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "background", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  g_assert (arg_enable == g_key_file_get_boolean (keyfile, "background", "autostart", NULL));

  xdp_dbus_impl_background_complete_enable_autostart (object, invocation, TRUE);

  return TRUE;
}

void
background_init (GDBusConnection *connection,
                 const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_background_skeleton_new ());

  g_signal_connect (helper, "handle-get-app-state", G_CALLBACK (handle_get_app_state), NULL);
  g_signal_connect (helper, "handle-notify-background", G_CALLBACK (handle_notify_background), NULL);
  g_signal_connect (helper, "handle-enable-autostart", G_CALLBACK (handle_enable_autostart), NULL);

  if (!g_dbus_interface_skeleton_export (helper, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
