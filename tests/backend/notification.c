#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "notification.h"

static GFileMonitor *config_monitor;

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
                         GUnixFDList *fd_list,
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

  if (error)
    {
      g_prefix_error_literal (&error, "Notification backend");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  notification_s = g_key_file_get_string (keyfile, "notification", "data", NULL);
  notification = g_variant_parse (G_VARIANT_TYPE_VARDICT, notification_s, NULL, NULL, &error);
  if (error)
    {
      g_prefix_error_literal (&error, "Notification backend");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!g_variant_equal (notification, arg_notification))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "Notification backend: expected %s but got %s",
                                             g_variant_print (notification, TRUE),
                                             g_variant_print (arg_notification, TRUE));
      return TRUE;
    }

  if (g_key_file_get_boolean (keyfile, "backend", "expect-no-call", NULL))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "Notification backend: Adding the notification should had failed already in the front end");
      return TRUE;
    }

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

  xdp_dbus_impl_notification_complete_add_notification (object, invocation, NULL);

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

static void
update_supported_options (GFileMonitor *monitor,
                          GFile *file,
                          GFile *other_file,
                          GFileMonitorEvent event_type,
                          XdpDbusImplNotification *object)
{
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *options_s = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  if (!g_file_query_exists (file, NULL))
    return;

  path = g_file_get_path (file);

  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  options_s = g_key_file_get_string (keyfile, "notification", "supported-options", NULL);

  if (!options_s)
      return;

  options = g_variant_parse (G_VARIANT_TYPE_VARDICT, options_s, NULL, NULL, &error);
  g_assert_no_error (error);

  xdp_dbus_impl_notification_set_supported_options (object, options);
}

void
notification_init (GDBusConnection *bus,
                   const char *object_path)
{
  const char *dir;
  g_autoptr(GFile) config_file = NULL;
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_notification_skeleton_new ());

  xdp_dbus_impl_notification_set_version (XDP_DBUS_IMPL_NOTIFICATION (helper), 2);
  g_signal_connect (helper, "handle-add-notification", G_CALLBACK (handle_add_notification), NULL);
  g_signal_connect (helper, "handle-remove-notification", G_CALLBACK (handle_remove_notification), NULL);

  dir = g_getenv ("XDG_DATA_HOME");
  config_file = g_file_new_build_filename (dir, "notification", NULL);
  config_monitor = g_file_monitor_file (config_file, G_FILE_MONITOR_NONE, NULL, NULL);

  g_signal_connect (config_monitor,
                    "changed",
                    G_CALLBACK (update_supported_options),
                    helper);

  if (!g_dbus_interface_skeleton_export (helper, bus, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}

