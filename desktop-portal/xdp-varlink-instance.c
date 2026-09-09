/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-varlink-instance.h"

struct _XdpVarlinkInstance
{
  GObject parent_instance;

  char *id;
  XdpAppInfo *app_info;

  /* Not owned; removed on dispose, once the last connection drops it */
  GHashTable *instances;
};

G_DEFINE_FINAL_TYPE (XdpVarlinkInstance, xdp_varlink_instance, G_TYPE_OBJECT);

static void
xdp_varlink_instance_dispose (GObject *object)
{
  XdpVarlinkInstance *self = XDP_VARLINK_INSTANCE (object);

  if (self->instances)
    {
      g_hash_table_remove (self->instances, self->id);
      self->instances = NULL;
    }

  g_clear_object (&self->app_info);

  G_OBJECT_CLASS (xdp_varlink_instance_parent_class)->dispose (object);
}

/* The id backs the table key, so it outlives the removal dispose does */
static void
xdp_varlink_instance_finalize (GObject *object)
{
  XdpVarlinkInstance *self = XDP_VARLINK_INSTANCE (object);

  g_clear_pointer (&self->id, g_free);

  G_OBJECT_CLASS (xdp_varlink_instance_parent_class)->finalize (object);
}

static void
xdp_varlink_instance_class_init (XdpVarlinkInstanceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_varlink_instance_dispose;
  object_class->finalize = xdp_varlink_instance_finalize;
}

static void
xdp_varlink_instance_init (XdpVarlinkInstance *self)
{
}

XdpVarlinkInstance *
xdp_varlink_instance_new (GHashTable *instances,
                          const char *id,
                          XdpAppInfo *app_info)
{
  XdpVarlinkInstance *self;

  g_return_val_if_fail (instances != NULL, NULL);
  g_return_val_if_fail (id != NULL, NULL);
  g_return_val_if_fail (XDP_IS_APP_INFO (app_info), NULL);

  self = g_object_new (XDP_TYPE_VARLINK_INSTANCE, NULL);
  self->id = g_strdup (id);
  self->app_info = g_object_ref (app_info);
  self->instances = instances;

  g_hash_table_insert (instances, self->id, self);

  return self;
}

const char *
xdp_varlink_instance_get_id (XdpVarlinkInstance *self)
{
  g_return_val_if_fail (XDP_IS_VARLINK_INSTANCE (self), NULL);

  return self->id;
}

XdpAppInfo *
xdp_varlink_instance_get_app_info (XdpVarlinkInstance *self)
{
  g_return_val_if_fail (XDP_IS_VARLINK_INSTANCE (self), NULL);

  return self->app_info;
}
