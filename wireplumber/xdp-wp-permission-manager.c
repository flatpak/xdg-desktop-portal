#include "config.h"

#include <glib-object.h>
#include <gio/gio.h>
#include <wp/wp.h>

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/permission.h>

#include "xdp-wp-permission-manager.h"
#include "xdp-impl-dbus.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-permission-manager")

struct _XdpWpPermissionManager
{
  GObject parent_instance;

  GDBusConnection *connection;
  WpCore *core;

  GCancellable *cancellable;

  XdpDbusImplPermissionStore *permission_store;
  gulong permission_changed_signal_id;

  WpObjectManager *camera_manager;
  gulong camera_added_signal_id;

  WpObjectManager *client_manager;
  gulong client_added_signal_id;
};

G_DEFINE_FINAL_TYPE (XdpWpPermissionManager, xdp_wp_permission_manager, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_CORE,
  PROP_CONNECTION,
  N_PROPS,
};

static GParamSpec *props[N_PROPS] = { NULL, };

#define PW_KEY_ACCESS_XDP_APP_ID PW_KEY_ACCESS ".xdg-desktop-portal.app_id"
#define PW_KEY_ACCESS_XDP_MEDIA_ROLES PW_KEY_ACCESS ".xdg-desktop-portal.media_roles"

static void
update_client_camera_permission (XdpWpPermissionManager *self,
                                 WpClient               *client,
                                 GVariantIter           *iter)
{
  g_autofree char *value = NULL;
  gboolean allowed;
  const char *roles = NULL;
  g_auto(GStrv) rolesv = NULL;
  g_autoptr (WpIterator) node_iter = NULL;
  g_auto (GValue) node_value = G_VALUE_INIT;

  allowed =
    iter && g_variant_iter_next (iter, "s", &value) && (g_strcmp0 (value, "yes") == 0);

  roles = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (client),
                                           PW_KEY_ACCESS_XDP_MEDIA_ROLES);

  if (!roles)
    {
      wp_warning_object (client, "Client has no media roles set");
      return;
    }

  rolesv = g_strsplit (roles, ",", 0);

  if (!g_strv_contains ((const gchar* const*) rolesv, "Camera"))
    return;

  wp_info_object (client, "Update camera permissions");

  wp_client_update_permissions (client, 1, wp_proxy_get_bound_id (WP_PROXY (client)),
                                allowed ? PW_PERM_ALL : 0);

  node_iter = wp_object_manager_new_iterator (self->camera_manager);
  while (wp_iterator_next (node_iter, &node_value))
    {
      WpNode *node = g_value_get_object (&node_value);

      wp_client_update_permissions (client, 1, wp_proxy_get_bound_id (WP_PROXY (node)),
                                    allowed ? PW_PERM_ALL : 0);

      g_value_unset (&node_value);
    }
}

static void
update_camera_permissions (XdpWpPermissionManager *self,
                           WpClient               *client,
                           GVariant               *permissions)
{
  g_autoptr (GVariant) perms = NULL;
  const char *app_id = NULL;

  if (!permissions)
    {
      g_autoptr (GVariant) data = NULL;
      g_autoptr (GError) error = NULL;

      if (!xdp_dbus_impl_permission_store_call_lookup_sync (self->permission_store,
                                                            "devices",
                                                            "camera",
                                                            &perms,
                                                            &data,
                                                            self->cancellable,
                                                            &error))
        {
          wp_warning_object (self, "Failed to lookup camera permission: %s", error->message);
          return;
        }

      permissions = perms;
    }

  g_assert (permissions != NULL);

  if (client)
    {
      g_autoptr (GVariantIter) value = NULL;

      app_id = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (client),
                                                PW_KEY_ACCESS_XDP_APP_ID);

      if (app_id)
        {
          g_variant_lookup (permissions, app_id, "as", &value);
          update_client_camera_permission (self, client, value);
          wp_info_object (client, "Camera permission for '%s' updated", app_id);
        }
      else
        {
          wp_warning_object (client, "Client has no app id set");
        }
    }
  else
    {
      g_autoptr (WpIterator) iter = NULL;
      g_auto (GValue) iter_value = G_VALUE_INIT;

      iter = wp_object_manager_new_iterator (self->client_manager);
      while (wp_iterator_next (iter, &iter_value))
        {
          WpClient *client = g_value_get_object (&iter_value);
          g_autoptr (GVariantIter) value = NULL;

          app_id = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (client),
                                                    PW_KEY_ACCESS_XDP_APP_ID);

          if (app_id)
            {
              g_variant_lookup (permissions, app_id, "as", &value);
              update_client_camera_permission (self, client, value);
              wp_info_object (client, "Camera permission for '%s' updated", app_id);
            }
          else
            {
              wp_warning_object (client, "Client has no app id set");
            }

          g_value_unset (&iter_value);
        }
    }
}

static void
on_permission_changed_cb (XdpWpPermissionManager     *self,
                          const char                 *arg_table,
                          const char                 *arg_id,
                          gboolean                    arg_deleted,
                          GVariant                   *arg_data,
                          GVariant                   *arg_permissions,
                          XdpDbusImplPermissionStore *permission_store)
{
  if (g_strcmp0 (arg_table, "devices") || g_strcmp0 (arg_id, "camera"))
    return;

  update_camera_permissions (self, NULL, arg_permissions);
}

static void
on_camera_added_cb (XdpWpPermissionManager *self,
                    WpNode                 *node,
                    WpObjectManager        *manager)
{
  update_camera_permissions (self, NULL, NULL);
}

static void
on_client_added_cb (XdpWpPermissionManager *self,
                    WpClient               *client,
                    WpObjectManager        *manager)
{
  if (!self->camera_manager)
    {
      wp_info_object (self,
                      "Granting ALL access to client %u",
                      wp_proxy_get_bound_id (WP_PROXY (client)));
      wp_client_update_permissions (client, 1, PW_ID_ANY, PW_PERM_ALL);
      return;
    }

  update_camera_permissions (self, client, NULL);
}

static void
xdp_wp_permission_manager_set_property (GObject      *object,
                                        guint         property_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  XdpWpPermissionManager *self = XDP_WP_PERMISSION_MANAGER (object);

  switch (property_id)
    {
    case PROP_CORE:
      self->core = g_object_ref (g_value_get_object (value));
      break;

    case PROP_CONNECTION:
      self->connection = g_object_ref (g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
xdp_wp_permission_manager_constructed (GObject *object)
{
  XdpWpPermissionManager *self = XDP_WP_PERMISSION_MANAGER (object);
  g_autoptr (GError) error = NULL;

  g_assert (WP_IS_CORE (self->core));
  g_assert (G_IS_DBUS_CONNECTION (self->connection));

  self->client_manager = wp_object_manager_new ();
  wp_object_manager_add_interest (self->client_manager,
                                  WP_TYPE_CLIENT,
                                  WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                  PW_KEY_ACCESS,
                                  "=s",
#ifdef HAVE_PW_XDP
                                  "xdg-desktop-portal",
#else
                                  "portal",
#endif
                                  WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                  PW_KEY_ACCESS_XDP_APP_ID,
                                  "+",
                                  NULL);

  self->client_added_signal_id = g_signal_connect_swapped (self->client_manager,
                                                           "object-added",
                                                           G_CALLBACK (on_client_added_cb),
                                                           self);

  self->permission_store =
    xdp_dbus_impl_permission_store_proxy_new_sync (self->connection,
                                                   G_DBUS_PROXY_FLAGS_NONE,
                                                   "org.freedesktop.impl.portal.PermissionStore",
                                                   "/org/freedesktop/impl/portal/PermissionStore",
                                                   NULL,
                                                   &error);

  if (!self->permission_store)
    {
      wp_warning_object (self,
                         "Failed to create permission store proxy: %s",
                         error->message);
      wp_core_install_object_manager (self->core, self->client_manager);
      return;
    }

  self->camera_manager = wp_object_manager_new ();
  wp_object_manager_add_interest (self->camera_manager,
                                  WP_TYPE_NODE,
                                  WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_ROLE, "=s", "Camera",
                                  WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "=s", "Video/Source",
                                  NULL);

  self->camera_added_signal_id = g_signal_connect_swapped (self->camera_manager,
                                                           "object-added",
                                                           G_CALLBACK (on_camera_added_cb),
                                                           self);

  wp_core_install_object_manager (self->core, self->camera_manager);
  wp_core_install_object_manager (self->core, self->client_manager);

  self->permission_changed_signal_id = g_signal_connect_swapped (self->permission_store,
                                                                 "changed",
                                                                 G_CALLBACK (on_permission_changed_cb),
                                                                 self);
}

static void
xdp_wp_permission_manager_dispose (GObject *object)
{
  XdpWpPermissionManager *self = XDP_WP_PERMISSION_MANAGER (object);

  g_clear_signal_handler (&self->camera_added_signal_id, self->camera_manager);
  g_clear_signal_handler (&self->client_added_signal_id, self->client_manager);

  g_clear_signal_handler (&self->permission_changed_signal_id, self->permission_store);

  g_cancellable_cancel (self->cancellable);
}

static void
xdp_wp_permission_manager_finalize (GObject *object)
{
  XdpWpPermissionManager *self = XDP_WP_PERMISSION_MANAGER (object);

  g_clear_object (&self->client_manager);
  g_clear_object (&self->camera_manager);

  g_clear_object (&self->permission_store);

  g_clear_object (&self->connection);
  g_clear_object (&self->core);

  g_clear_object (&self->cancellable);
}

static void
xdp_wp_permission_manager_class_init (XdpWpPermissionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = xdp_wp_permission_manager_set_property;
  object_class->constructed = xdp_wp_permission_manager_constructed;
  object_class->dispose = xdp_wp_permission_manager_dispose;
  object_class->finalize = xdp_wp_permission_manager_finalize;

  props[PROP_CORE] = g_param_spec_object ("core", NULL, NULL,
                                          WP_TYPE_CORE,
                                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_CONNECTION] = g_param_spec_object ("connection", NULL, NULL,
                                                G_TYPE_DBUS_CONNECTION,
                                                G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
xdp_wp_permission_manager_init (XdpWpPermissionManager *self)
{
  self->cancellable = g_cancellable_new ();
}

XdpWpPermissionManager *
xdp_wp_permission_manager_new (WpCore          *core,
                               GDBusConnection *connection)
{
  return g_object_new (XDP_WP_TYPE_PERMISSION_MANAGER,
                       "core", core,
                       "connection", connection,
                       NULL);
}
