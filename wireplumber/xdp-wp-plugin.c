#include <wp/wp.h>

#include "xdp-wp-permission-manager.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-plugin")

/* Copied from dbus-connection-state.h */
typedef enum {
  WP_DBUS_CONNECTION_STATE_CLOSED = 0,
  WP_DBUS_CONNECTION_STATE_CONNECTING,
  WP_DBUS_CONNECTION_STATE_CONNECTED,
} WpDBusConnectionState;

G_DECLARE_FINAL_TYPE (XdpWpPlugin, xdp_wp_plugin, XDP_WP, PLUGIN, WpPlugin)

struct _XdpWpPlugin
{
  WpPlugin parent_instance;

  WpPlugin *dbus_connection_plugin;
  gulong dbus_changed_signal_id;

  XdpWpPermissionManager *permission_manager;
};

G_DEFINE_FINAL_TYPE (XdpWpPlugin, xdp_wp_plugin, WP_TYPE_PLUGIN)

static void
on_dbus_connection_plugin_state_changed (XdpWpPlugin *self,
                                         GParamSpec  *pspec,
                                         GObject     *object)
{
  WpDBusConnectionState state = -1;
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (WpCore) core = NULL;

  g_object_get (self->dbus_connection_plugin, "state", &state, NULL);

  if (state != WP_DBUS_CONNECTION_STATE_CONNECTED)
    {
      g_clear_object (&self->permission_manager);
      return;
    }

  g_object_get (self->dbus_connection_plugin, "connection", &connection, NULL);
  g_return_if_fail (connection != NULL);

  core = wp_object_get_core (WP_OBJECT (self));

  if (core)
    {
      self->permission_manager = xdp_wp_permission_manager_new (core, connection);
      wp_debug_object (self, "Permission manager created");
    }
  else
    {
      wp_debug_object (self, "No core returned");
    }
}

static void
xdp_wp_plugin_enable (WpPlugin     *plugin,
                      WpTransition *transition)
{
  XdpWpPlugin *self = XDP_WP_PLUGIN (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  self->dbus_connection_plugin = wp_plugin_find (core, "dbus-connection");
  if (!self->dbus_connection_plugin)
    {
      wp_transition_return_error (transition, g_error_new (WP_DOMAIN_LIBRARY,
                                                           WP_LIBRARY_ERROR_INVARIANT,
                                                           "dbus-connection module must be loaded before xdp-desktop-portal"));
      return;
    }

  self->dbus_changed_signal_id =
    g_signal_connect_swapped (self->dbus_connection_plugin,
                              "notify::state",
                              G_CALLBACK (on_dbus_connection_plugin_state_changed),
                              self);
  on_dbus_connection_plugin_state_changed (self, NULL, NULL);

  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
xdp_wp_plugin_disable (WpPlugin *plugin)
{
  XdpWpPlugin *self = XDP_WP_PLUGIN (plugin);

  g_clear_object (&self->permission_manager);

  g_signal_handler_disconnect (self->dbus_connection_plugin,
                               self->dbus_changed_signal_id);
  g_clear_object (&self->dbus_connection_plugin);

  wp_object_update_features (WP_OBJECT (self), 0, WP_PLUGIN_FEATURE_ENABLED);
}

static void
xdp_wp_plugin_class_init (XdpWpPluginClass *klass)
{
  WpPluginClass *plugin_class = WP_PLUGIN_CLASS (klass);

  plugin_class->enable = xdp_wp_plugin_enable;
  plugin_class->disable = xdp_wp_plugin_disable;
}

static void
xdp_wp_plugin_init (XdpWpPlugin *self)
{
}

WP_PLUGIN_EXPORT GObject *
wireplumber__module_init (WpCore     *core,
                          WpSpaJson  *args,
                          GError    **error)
{
  return g_object_new (xdp_wp_plugin_get_type (),
                       "name", "xdp-desktop-portal",
                       "core", core,
                       NULL);
}
