/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#include "xdp-wp-metadata.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/permission.h>
#include <wp/wp.h>

#include "xdp-pw-keys.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-metadata");

struct _XdpWpMetadata
{
  GObject parent_instance;

  WpCore *core;
  WpMetadata *metadata;

  WpObjectManager *camera_om;
  gulong camera_changed_id;

  GAsyncReadyCallback init_callback;
  gpointer init_user_data;

  WpObjectManager *xdp_daemon_om;
  gulong xdp_daemon_added_id;
};

static void async_initable_iface_init (GAsyncInitableIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpWpMetadata, xdp_wp_metadata, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init));

typedef enum
{
  PROP_CORE = 1,
  PROP_CAMERA_OM,
} XdpWpMetadataProps;

static GParamSpec *props[PROP_CAMERA_OM + 1] = { NULL, };

static void
on_xdp_daemon_added (XdpWpMetadata   *self,
                     WpClient        *client,
                     WpObjectManager *manager)
{
  struct pw_permission permissions[3];

  g_assert (XDP_WP_IS_METADATA (self));
  g_return_if_fail (WP_IS_CLIENT (client));

  permissions[0] = PW_PERMISSION_INIT (PW_ID_CORE, PW_PERM_R);
  permissions[1] = PW_PERMISSION_INIT (wp_proxy_get_bound_id (WP_PROXY (self->metadata)),
                                       PW_PERM_R);
  permissions[2] = PW_PERMISSION_INIT (PW_ID_ANY, 0);

  wp_client_update_permissions_array (client, G_N_ELEMENTS (permissions), permissions);

  wp_info_object (client, "Daemon client permission updated");
}

static void
on_camera_changed (WpObjectManager *manager,
                   WpMetadata      *metadata)
{
  g_assert (WP_IS_METADATA (metadata));
  const char *value = NULL;

  g_assert (WP_IS_METADATA (metadata));

  value = wp_object_manager_get_n_objects (manager) != 0 ? "true" : "false";

  wp_metadata_set (metadata, PW_ID_CORE, XDP_PW_KEY_CAMERA_PRESENT, "Spa:Bool", value);

  wp_debug_object (metadata, XDP_PW_KEY_CAMERA_PRESENT " set to %s", value);
}

static void
on_metadata_activated (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (XdpWpMetadata) self = XDP_WP_METADATA (user_data);

  g_assert (g_direct_equal (self->metadata, object));

  self->init_callback (G_OBJECT (self), result, self->init_user_data);
  self->init_callback = NULL;
  self->init_user_data = NULL;
}

static void
xdp_wp_metadata_init_async (GAsyncInitable      *initable,
                            int                  io_priority,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  XdpWpMetadata *self = XDP_WP_METADATA (initable);
  WpObjectInterest *interest;
  GClosure *closure;

  g_assert (WP_IS_CORE (self->core));

  wp_debug_object (self, "Initializing metadata");

  self->metadata = WP_METADATA (wp_impl_metadata_new_full (self->core,
                                                           XDP_PW_METADATA_NAME,
                                                           NULL));

  interest = wp_object_interest_new_type (WP_TYPE_CLIENT);
  wp_object_interest_add_constraint (interest,
                                     WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                     PW_KEY_ACCESS,
                                     WP_CONSTRAINT_VERB_EQUALS,
                                     g_variant_new_string (XDP_PW_ACCESS));
  wp_object_interest_add_constraint (interest,
                                     WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                     XDP_PW_KEY_APP_ID,
                                     WP_CONSTRAINT_VERB_IS_ABSENT,
                                     NULL);
  wp_object_interest_add_constraint (interest,
                                     WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                     XDP_PW_KEY_DAEMON,
                                     WP_CONSTRAINT_VERB_EQUALS,
                                     g_variant_new_boolean (TRUE));
  g_assert (wp_object_interest_validate (interest, NULL));

  self->xdp_daemon_om = wp_object_manager_new ();
  wp_object_manager_add_interest_full (self->xdp_daemon_om, g_steal_pointer (&interest));

  closure = g_cclosure_new_object (G_CALLBACK (on_metadata_activated),
                                   G_OBJECT (g_object_ref (self)));
  self->init_callback = callback;
  self->init_user_data = user_data;

  wp_debug_object (self, "Activating metadata");
  wp_object_activate_closure (WP_OBJECT (self->metadata),
                              WP_OBJECT_FEATURES_ALL,
                              cancellable,
                              closure);
}

static gboolean
xdp_wp_metadata_init_finish (GAsyncInitable  *initable,
                             GAsyncResult    *res,
                             GError         **error)
{
  XdpWpMetadata *self = XDP_WP_METADATA (initable);

  if (G_UNLIKELY (!wp_object_activate_finish (WP_OBJECT (self->metadata), res, error)))
    {
      g_prefix_error_literal (error, "Failed to activate: ");
      return FALSE;
    }

  wp_debug_object (self, "Metadata initialized and activated");

  self->xdp_daemon_added_id = g_signal_connect_swapped (self->xdp_daemon_om,
                                                        "object-added",
                                                        G_CALLBACK (on_xdp_daemon_added),
                                                        self);
  wp_core_install_object_manager (self->core, self->xdp_daemon_om);

  self->camera_changed_id = g_signal_connect (self->camera_om,
                                              "objects-changed",
                                              G_CALLBACK (on_camera_changed),
                                              self->metadata);

  return TRUE;
}

static void
async_initable_iface_init (GAsyncInitableIface *iface)
{
  iface->init_async = xdp_wp_metadata_init_async;
  iface->init_finish = xdp_wp_metadata_init_finish;
}

static void
xdp_wp_metadata_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  XdpWpMetadata *self = XDP_WP_METADATA (object);

  switch ((XdpWpMetadataProps)property_id)
    {
    case PROP_CORE:
      self->core = g_object_ref (g_value_get_object (value));
      break;

    case PROP_CAMERA_OM:
      self->camera_om = g_object_ref (g_value_get_object (value));
      break;
    }
}

static void
xdp_wp_metadata_dispose (GObject *object)
{
  XdpWpMetadata *self = XDP_WP_METADATA (object);

  wp_debug_object (self, "Disposing metadata");

  g_clear_signal_handler (&self->camera_changed_id, self->camera_om);
  g_clear_signal_handler (&self->xdp_daemon_added_id, self->xdp_daemon_om);

  /* Clear daemon clients from metadata permissions since metadata object id
   * might be re-used and represent another object. */
  if (wp_object_manager_is_installed (self->xdp_daemon_om))
    {
      g_autoptr (WpIterator) iter = NULL;
      g_auto (GValue) value = G_VALUE_INIT;
      uint32_t bound_id = wp_proxy_get_bound_id (WP_PROXY (self->metadata));

      iter = wp_object_manager_new_iterator (self->xdp_daemon_om);
      for (; wp_iterator_next (iter, &value); g_value_unset (&value))
        {
          WpClient *client = g_value_get_object (&value);

          if (G_UNLIKELY (!WP_IS_CLIENT (client)))
            continue;

          wp_client_update_permissions (client, 1, bound_id, 0);
        }
    }

  G_OBJECT_CLASS (xdp_wp_metadata_parent_class)->dispose (object);
}

static void
xdp_wp_metadata_finalize (GObject *object)
{
  XdpWpMetadata *self = XDP_WP_METADATA (object);

  wp_debug_object (self, "Finalizing metadata");

  g_clear_object (&self->xdp_daemon_om);

  g_clear_object (&self->camera_om);

  g_clear_object (&self->metadata);
  g_clear_object (&self->core);

  G_OBJECT_CLASS (xdp_wp_metadata_parent_class)->finalize (object);
}

static void
xdp_wp_metadata_class_init (XdpWpMetadataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = xdp_wp_metadata_set_property;
  object_class->dispose = xdp_wp_metadata_dispose;
  object_class->finalize = xdp_wp_metadata_finalize;

  props[PROP_CORE] = g_param_spec_object ("core", NULL, NULL,
                                          WP_TYPE_CORE,
                                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_CAMERA_OM] = g_param_spec_object ("camera-om", NULL, NULL,
                                               WP_TYPE_OBJECT_MANAGER,
                                               G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

static void
xdp_wp_metadata_init (XdpWpMetadata *self)
{
}

void
xdp_wp_metadata_new (WpCore              *core,
                     WpObjectManager     *camera_om,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  g_assert (WP_IS_CORE (core));
  g_assert (WP_IS_OBJECT_MANAGER (camera_om));

  g_async_initable_new_async (XDP_WP_TYPE_METADATA,
                              G_PRIORITY_DEFAULT,
                              cancellable, callback, user_data,
                              "core", core,
                              "camera-om", camera_om,
                              NULL);
}

XdpWpMetadata *
xdp_wp_metadata_new_finish (GObject       *object,
                            GAsyncResult  *res,
                            GError       **error)
{
  GObject *ret = g_async_initable_new_finish (G_ASYNC_INITABLE (object), res, error);

  if (G_UNLIKELY (object == NULL))
    return NULL;

  g_assert (XDP_WP_IS_METADATA (ret));

  return XDP_WP_METADATA (ret);
}
