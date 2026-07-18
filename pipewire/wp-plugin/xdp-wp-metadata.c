/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#include "xdp-wp-metadata.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <wp/wp.h>

#include "xdp-pw-keys.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-metadata");

struct _XdpWpMetadata
{
  GObject parent_instance;

  WpCore *core;
  WpMetadata *metadata;

  GAsyncReadyCallback init_callback;
  gpointer init_user_data;
};

static void async_initable_iface_init (GAsyncInitableIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpWpMetadata, xdp_wp_metadata, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init));

typedef enum
{
  PROP_CORE = 1,
} XdpWpMetadataProps;

static GParamSpec *props[PROP_CORE + 1] = { NULL, };

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
  GClosure *closure;

  g_assert (WP_IS_CORE (self->core));

  wp_debug_object (self, "Initializing metadata");

  self->metadata = WP_METADATA (wp_impl_metadata_new_full (self->core,
                                                           XDP_PW_METADATA_NAME,
                                                           NULL));

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
    }
}

static void
xdp_wp_metadata_finalize (GObject *object)
{
  XdpWpMetadata *self = XDP_WP_METADATA (object);

  wp_debug_object (self, "Finalizing metadata");

  g_clear_object (&self->metadata);
  g_clear_object (&self->core);

  G_OBJECT_CLASS (xdp_wp_metadata_parent_class)->finalize (object);
}

static void
xdp_wp_metadata_class_init (XdpWpMetadataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = xdp_wp_metadata_set_property;
  object_class->finalize = xdp_wp_metadata_finalize;

  props[PROP_CORE] = g_param_spec_object ("core", NULL, NULL,
                                          WP_TYPE_CORE,
                                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

static void
xdp_wp_metadata_init (XdpWpMetadata *self)
{
}

void
xdp_wp_metadata_new (WpCore              *core,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  g_assert (WP_IS_CORE (core));

  g_async_initable_new_async (XDP_WP_TYPE_METADATA,
                              G_PRIORITY_DEFAULT,
                              cancellable, callback, user_data,
                              "core", core,
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
