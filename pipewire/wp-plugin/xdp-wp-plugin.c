/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#include <pipewire/keys.h>
#include <wp/wp.h>

#include "xdp-wp-metadata.h"
#include "xdp-wp-permission-manager.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-plugin");

/* Copied from dbus-connection-state.h */
typedef enum {
  WP_DBUS_CONNECTION_STATE_CLOSED = 0,
  WP_DBUS_CONNECTION_STATE_CONNECTING,
  WP_DBUS_CONNECTION_STATE_CONNECTED,
} WpDBusConnectionState;

G_DECLARE_FINAL_TYPE (XdpWpPlugin, xdp_wp_plugin, XDP_WP, PLUGIN, WpPlugin);

struct _XdpWpPlugin
{
  WpPlugin parent_instance;

  GCancellable *cancellable;

  WpPlugin *dbus_connection_plugin;
  gulong dbus_changed_signal_id;

  XdpWpMetadata *metadata;
  WpObjectManager *camera_om;
  XdpWpPermissionManager *permission_manager;
};

G_DEFINE_FINAL_TYPE (XdpWpPlugin, xdp_wp_plugin, WP_TYPE_PLUGIN);

static void
on_dbus_connection_plugin_state_changed (XdpWpPlugin *self,
                                         GParamSpec  *pspec,
                                         GObject     *object)
{
  WpDBusConnectionState state = -1;
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  g_object_get (self->dbus_connection_plugin, "state", &state, NULL);

  if (state != WP_DBUS_CONNECTION_STATE_CONNECTED)
    {
      g_clear_object (&self->permission_manager);
      wp_debug_object (self, "Permission manager cleared");
      return;
    }

  g_object_get (self->dbus_connection_plugin, "connection", &connection, NULL);
  g_return_if_fail (connection != NULL);

  self->permission_manager = xdp_wp_permission_manager_new (core,
                                                            self->camera_om,
                                                            connection);
  wp_debug_object (self, "Permission manager created");
}

static void
metadata_init_cb (GObject      *object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  XdpWpPlugin *self = XDP_WP_PLUGIN (user_data);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));;
  g_autoptr (GError) error = NULL;

  self->metadata = xdp_wp_metadata_new_finish (object, res, &error);
  if (G_UNLIKELY (self->metadata == NULL))
    {
      wp_critical_object (self, "Error while creating metadata: %s", error->message);
      return;
    }

  wp_core_install_object_manager (core, self->camera_om);

  self->dbus_changed_signal_id =
    g_signal_connect_swapped (self->dbus_connection_plugin,
                              "notify::state",
                              G_CALLBACK (on_dbus_connection_plugin_state_changed),
                              self);
  on_dbus_connection_plugin_state_changed (self, NULL, NULL);
}

static void
xdp_wp_plugin_enable (WpPlugin     *plugin,
                      WpTransition *transition)
{
  XdpWpPlugin *self = XDP_WP_PLUGIN (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));
  WpObjectInterest *interest;

  self->cancellable = g_cancellable_new ();

  self->dbus_connection_plugin = wp_plugin_find (core, "dbus-connection");
  if (!self->dbus_connection_plugin)
    {
      wp_transition_return_error (transition, g_error_new (WP_DOMAIN_LIBRARY,
                                                           WP_LIBRARY_ERROR_INVARIANT,
                                                           "dbus-connection module must be loaded before xdp-desktop-portal"));
      return;
    }

  interest = wp_object_interest_new_type (WP_TYPE_NODE);
  wp_object_interest_add_constraint (interest,
                                     WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                     PW_KEY_MEDIA_ROLE,
                                     WP_CONSTRAINT_VERB_EQUALS,
                                     g_variant_new_string ("Camera"));
  wp_object_interest_add_constraint (interest,
                                     WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                     PW_KEY_MEDIA_CLASS,
                                     WP_CONSTRAINT_VERB_EQUALS,
                                     g_variant_new_string ("Video/Source"));
  g_assert (wp_object_interest_validate (interest, NULL));

  self->camera_om = wp_object_manager_new ();
  wp_object_manager_add_interest_full (self->camera_om, g_steal_pointer (&interest));

  xdp_wp_metadata_new (core,
                       self->camera_om,
                       self->cancellable,
                       metadata_init_cb,
                       self);

  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
xdp_wp_plugin_disable (WpPlugin *plugin)
{
  XdpWpPlugin *self = XDP_WP_PLUGIN (plugin);

  g_cancellable_cancel (self->cancellable);

  g_signal_handler_disconnect (self->dbus_connection_plugin,
                               self->dbus_changed_signal_id);

  g_clear_object (&self->permission_manager);
  g_clear_object (&self->metadata);

  g_clear_object (&self->dbus_connection_plugin);
  g_clear_object (&self->cancellable);

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
