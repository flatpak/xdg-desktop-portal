#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "notification.h"

typedef struct {
  XdpDbusImplNotification *impl;
  char *app_id;
  char *id;
  char *action;
} ActionData;

static gboolean
invoke_action (gpointer data)
{
  ActionData *adata = data;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));

  g_print ("emitting ActionInvoked\n");
  xdp_dbus_impl_notification_emit_action_invoked (adata->impl,
                                                  adata->app_id,
                                                  adata->id,
                                                  adata->action,
                                                  g_variant_builder_end (&builder));

  g_free (adata->app_id);
  g_free (adata->id);
  g_free (adata->action);
  g_free (adata);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_add_notification (XdpDbusImplNotification *object,
                         GDBusMethodInvocation *invocation,
                         const gchar *arg_app_id,
                         const gchar *arg_id,
                         GVariant *arg_notification)
{
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *notification_s = NULL;
  g_autoptr(GVariant) notification = NULL;
  g_autoptr(GError) error = NULL;
  int delay;

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "notification", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  notification_s = g_key_file_get_string (keyfile, "notification", "data", NULL);
  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (g_variant_equal (notification, arg_notification));

  if (g_key_file_get_boolean (keyfile, "backend", "expect-no-call", NULL))
    g_assert_not_reached ();

  delay = g_key_file_get_integer (keyfile, "backend", "delay", NULL);
  if (delay != 0)
    {
      ActionData *data;
      data = g_new (ActionData, 1);
      data->impl = object;
      data->app_id = g_strdup (arg_app_id);
      data->id = g_strdup (arg_id);
      data->action = g_key_file_get_string (keyfile, "notification", "action", NULL);

      g_timeout_add (delay, invoke_action, data);
    }

  xdp_dbus_impl_notification_complete_add_notification (object, invocation);

  return TRUE;
}

static gboolean
handle_remove_notification (XdpDbusImplNotification *object,
                            GDBusMethodInvocation *invocation,
                            const gchar *arg_app_id,
                            const gchar *arg_id)
{
  xdp_dbus_impl_notification_complete_remove_notification (object, invocation);

  return TRUE;
}

void
notification_init (GDBusConnection *bus,
                   const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_notification_skeleton_new ());

  g_signal_connect (helper, "handle-add-notification", G_CALLBACK (handle_add_notification), NULL);
  g_signal_connect (helper, "handle-remove-notification", G_CALLBACK (handle_remove_notification), NULL);

  if (!g_dbus_interface_skeleton_export (helper, bus, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}

