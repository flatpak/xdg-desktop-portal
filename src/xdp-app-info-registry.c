/*
 * Copyright © 2025 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "xdp-app-info.h"

#include "xdp-app-info-registry.h"

typedef struct _XdpAppInfoRegistryItem
{
  gatomicrefcount ref_count;
  XdpAppInfo *app_info;
  GMutex mutex;
} XdpAppInfoRegistryItem;

struct _XdpAppInfoRegistry
{
  GObject parent_instance;

  GHashTable *app_infos; /* unique dbus name -> XdpAppInfoRegistryItem */
  GMutex app_infos_mutex;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoRegistry,
                     xdp_app_info_registry,
                     G_TYPE_OBJECT)

static XdpAppInfoRegistryItem *
xdp_app_info_registry_item_new (void)
{
  XdpAppInfoRegistryItem *item;

  item = g_new0 (XdpAppInfoRegistryItem, 1);
  g_mutex_init (&item->mutex);
  g_atomic_ref_count_init (&item->ref_count);

  return item;
}

static XdpAppInfoRegistryItem *
xdp_app_info_registry_item_ref (XdpAppInfoRegistryItem *item)
{
  g_atomic_ref_count_inc (&item->ref_count);

  return item;
}

static void
xdp_app_info_registry_item_unref (XdpAppInfoRegistryItem *item)
{
  if (g_atomic_ref_count_dec (&item->ref_count))
    {
      g_clear_object (&item->app_info);
      g_mutex_clear (&item->mutex);
      g_free (item);
    }
}

static void
xdp_app_info_registry_item_lock (XdpAppInfoRegistryItem *item)
{
  g_mutex_lock (&item->mutex);
}

static void
xdp_app_info_registry_item_unlock (XdpAppInfoRegistryItem *item)
{
  g_mutex_unlock (&item->mutex);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpAppInfoRegistryItem,
                               xdp_app_info_registry_item_unref)

static void
xdp_app_info_registry_dispose (GObject *object)
{
  XdpAppInfoRegistry *registry = XDP_APP_INFO_REGISTRY (object);

  if (registry->app_infos)
    {
      g_mutex_clear (&registry->app_infos_mutex);
      g_clear_pointer (&registry->app_infos, g_hash_table_unref);
    }

  G_OBJECT_CLASS (xdp_app_info_registry_parent_class)->dispose (object);
}

static void
xdp_app_info_registry_class_init (XdpAppInfoRegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_registry_dispose;
}

static void
xdp_app_info_registry_init (XdpAppInfoRegistry *registry)
{
}

XdpAppInfoRegistry *
xdp_app_info_registry_new (void)
{
  XdpAppInfoRegistry *registry = g_object_new (XDP_TYPE_APP_INFO_REGISTRY, NULL);

  registry->app_infos = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free,
                                               (GDestroyNotify) xdp_app_info_registry_item_unref);
  g_mutex_init (&registry->app_infos_mutex);

  return registry;
}

static XdpAppInfoRegistryItem *
xdp_app_info_registry_ensure_item (XdpAppInfoRegistry *registry,
                                   const char         *sender)
{
  g_autoptr(XdpAppInfoRegistryItem) item = NULL;
  XdpAppInfoRegistryItem *match;

  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  match = g_hash_table_lookup (registry->app_infos, sender);
  if (match)
    return xdp_app_info_registry_item_ref (match);

  item = xdp_app_info_registry_item_new ();
  g_hash_table_insert (registry->app_infos,
                       g_strdup (sender),
                       xdp_app_info_registry_item_ref (item));

  return g_steal_pointer (&item);
}

XdpAppInfoRegistryLocker *
xdp_app_info_registry_lock (XdpAppInfoRegistry *registry,
                            const char         *sender)
{
  g_autoptr(XdpAppInfoRegistryItem) item =
    xdp_app_info_registry_ensure_item (registry, sender);

  xdp_app_info_registry_item_lock (item);
  return (XdpAppInfoRegistryLocker *) g_steal_pointer (&item);
}

void
xdp_app_info_registry_locker_free (XdpAppInfoRegistryLocker *locker)
{
  XdpAppInfoRegistryItem *item = (XdpAppInfoRegistryItem *) locker;

  xdp_app_info_registry_item_unlock (item);
  xdp_app_info_registry_item_unref (item);
}

void
xdp_app_info_registry_insert_unlocked (XdpAppInfoRegistry *registry,
                                       XdpAppInfo         *app_info)
{
  const char *sender = xdp_app_info_get_sender (app_info);
  g_autoptr(XdpAppInfoRegistryItem) item =
    xdp_app_info_registry_ensure_item (registry, sender);

  g_set_object (&item->app_info, app_info);
}

XdpAppInfo *
xdp_app_info_registry_lookup_unlocked (XdpAppInfoRegistry *registry,
                                       const char         *sender)
{
  g_autoptr(XdpAppInfoRegistryItem) item =
    xdp_app_info_registry_ensure_item (registry, sender);

  return item->app_info ? g_object_ref (item->app_info) : NULL;
}

XdpAppInfo *
xdp_app_info_registry_lookup_sender (XdpAppInfoRegistry *registry,
                                     const char         *sender)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  XdpAppInfoRegistryItem *match;

  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  match = g_hash_table_lookup (registry->app_infos, sender);
  if (!match)
    return NULL;

  if (!g_mutex_trylock (&match->mutex))
    return NULL;

  app_info = g_object_ref (match->app_info);
  g_mutex_unlock (&match->mutex);

  return g_steal_pointer (&app_info);
}

void
xdp_app_info_registry_delete (XdpAppInfoRegistry *registry,
                              const char         *sender)
{
  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  g_hash_table_remove (registry->app_infos, sender);
}

XdpAppInfo *
xdp_app_info_registry_ensure_for_invocation_sync (XdpAppInfoRegistry     *registry,
                                                  GDBusMethodInvocation  *invocation,
                                                  GCancellable           *cancellable,
                                                  GError                **error)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(XdpAppInfoRegistryLocker) locker = NULL;
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  locker = xdp_app_info_registry_lock (registry, sender);

  app_info = xdp_app_info_registry_lookup_unlocked (registry, sender);
  if (app_info)
    return g_steal_pointer (&app_info);

  app_info = xdp_app_info_new_for_invocation_sync (invocation,
                                                   cancellable,
                                                   error);
  if (app_info)
    xdp_app_info_registry_insert_unlocked (registry, app_info);

  return g_steal_pointer (&app_info);
}
