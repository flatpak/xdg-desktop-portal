/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-app-info-registry.h"

#include "xdp-app-info.h"
#include "xdp-dex.h"

struct _XdpAppInfoRegistry
{
  GObject parent_instance;

  GHashTable *app_infos; /* unique dbus name -> XdpAppInfo* */
  GHashTable *channels; /* unique dbus name -> DexChannel* */
  GMutex channels_lock; /* protects channels */
};

G_DEFINE_FINAL_TYPE (XdpAppInfoRegistry,
                     xdp_app_info_registry,
                     G_TYPE_OBJECT);

typedef enum _QueueDataKind {
  QUEUE_DATA_KIND_ENSURE,
  QUEUE_DATA_KIND_INSERT,
  QUEUE_DATA_KIND_DELETE,
} QueueDataKind;

typedef struct _QueueData
{
  DexPromise *promise;
  QueueDataKind kind;

  /* ensure, insert */
  GDBusMethodInvocation *invocation;
  DexFuture *app_info_future;

  /* delete */
  char *sender;
} QueueData;

static QueueData *
queue_data_copy (QueueData *data)
{
  QueueData *copy = g_new0 (QueueData, 1);

  copy->kind = data->kind;
  copy->promise = data->promise ? dex_ref (data->promise) : NULL;
  copy->invocation = data->invocation ? g_object_ref (data->invocation) : NULL;
  copy->app_info_future = data->app_info_future ? dex_ref (data->app_info_future) : NULL;
  copy->sender = g_strdup (data->sender);

  return copy;
}

static void
queue_data_free (QueueData *data)
{
  g_clear_pointer (&data->promise, dex_unref);
  g_clear_object (&data->invocation);
  g_clear_pointer (&data->app_info_future, dex_unref);
  g_clear_pointer (&data->sender, g_free);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (QueueData, queue_data_free)
G_DEFINE_BOXED_TYPE (QueueData, queue_data, queue_data_copy, queue_data_free)

static void
channel_unref_and_close (DexChannel *channel)
{
  dex_channel_close_send (channel);
  dex_unref (channel);
}

static void
xdp_app_info_registry_dispose (GObject *object)
{
  XdpAppInfoRegistry *registry = XDP_APP_INFO_REGISTRY (object);

  if (registry->channels)
    g_mutex_clear (&registry->channels_lock);

  g_clear_pointer (&registry->app_infos, g_hash_table_unref);
  g_clear_pointer (&registry->channels, g_hash_table_unref);


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
                                               g_object_unref);

  registry->channels = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free,
                                              (GDestroyNotify) channel_unref_and_close);

  g_mutex_init (&registry->channels_lock);

  return registry;
}

static DexFuture *
work_fiber (XdpAppInfoRegistry *registry,
            const char         *sender)
{
  g_autoptr(DexChannel) channel = NULL;

  g_mutex_lock (&registry->channels_lock);
  channel = dex_ref (g_hash_table_lookup (registry->channels, sender));
  g_mutex_unlock (&registry->channels_lock);

  while (TRUE)
    {
      g_autoptr(QueueData) data = NULL;
      g_autoptr(DexFuture) future = NULL;

      future = dex_channel_receive (channel);

      /* if we have to wait, the queue is empty and we shut down the channel */
      if (dex_future_is_pending (future))
        break;

      /* also shut down if the other side closed */
      data = dex_await_boxed (g_steal_pointer (&future), NULL);
      if (!data)
        break;

      switch (data->kind)
        {
        case QUEUE_DATA_KIND_ENSURE:
          {
            XdpAppInfo *app_info;

            app_info = g_hash_table_lookup (registry->app_infos, sender);
            if (!app_info)
              {
                g_autoptr(XdpAppInfo) app_info_owned = NULL;
                g_autoptr(GError) error = NULL;

                /* FIXME convert to future */
                app_info_owned = app_info =
                  xdp_app_info_new_for_invocation_sync (data->invocation,
                                                        NULL,
                                                        &error);
                if (!app_info_owned)
                  {
                    dex_promise_reject (data->promise, g_steal_pointer (&error));
                    break;
                  }

                g_hash_table_insert (registry->app_infos,
                                     g_strdup (sender),
                                     g_steal_pointer (&app_info_owned));
              }

            dex_promise_resolve_object (data->promise, g_object_ref (app_info));
          }
          break;
        case QUEUE_DATA_KIND_INSERT:
          {
            g_autoptr(GError) error = NULL;
            g_autoptr(XdpAppInfo) app_info = NULL;

            if (g_hash_table_contains (registry->app_infos, sender))
              {
                dex_promise_resolve_boolean (data->promise, FALSE);
                break;
              }

            app_info = dex_await_object (g_steal_pointer (&data->app_info_future),
                                         &error);
            if (!app_info)
              {
                g_debug ("Could not await app info for sender %s: %s",
                         sender, error->message);
                dex_promise_resolve_boolean (data->promise, FALSE);
                break;
              }

            g_debug ("Inserted app info %s for sender %s",
                     xdp_app_info_get_id (app_info), sender);
            g_hash_table_insert (registry->app_infos,
                                 g_strdup (sender),
                                 g_steal_pointer (&app_info));
            dex_promise_resolve_boolean (data->promise, TRUE);
          }
          break;
        case QUEUE_DATA_KIND_DELETE:
          {
            dex_promise_resolve_boolean (data->promise,
                                         g_hash_table_remove (registry->app_infos,
                                                              sender));
          }
          break;
        default:
          g_assert_not_reached ();
        }
    }

  g_mutex_lock (&registry->channels_lock);
  g_hash_table_remove (registry->channels, sender);
  g_mutex_unlock (&registry->channels_lock);

  return dex_future_new_true ();
}

/* all callers must hold the registry->channels_lock lock */
static DexFuture *
queue_work (XdpAppInfoRegistry *registry,
            const char         *sender,
            QueueData          *data)
{
  g_autoptr(QueueData) owned_data = data;
  DexChannel *channel = g_hash_table_lookup (registry->channels, sender);
  g_autoptr(DexChannel) channel_owned = NULL;
  g_autoptr(DexPromise) promise = dex_promise_new ();
  g_autoptr(DexFuture) future = NULL;

  if (!channel)
    channel = channel_owned = dex_channel_new (0);

  if (!dex_channel_can_send (channel))
    {
      dex_promise_reject (promise,
                          g_error_new (G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Channel closed"));
      return DEX_FUTURE (g_steal_pointer (&promise));
    }

  data->promise = dex_ref (promise);
  future = dex_channel_send (channel,
                             dex_future_new_take_boxed (queue_data_get_type (),
                                                        g_steal_pointer (&owned_data)));

  if (channel_owned)
    {
      g_autoptr(DexFuture) spawn_future = NULL;

      g_hash_table_insert (registry->channels,
                           g_strdup (sender),
                           g_steal_pointer (&channel_owned));

      spawn_future = dex_scheduler_spawn_closure (NULL, 0,
                                                  G_CALLBACK (work_fiber),
                                                  2,
                                                  XDP_TYPE_APP_INFO_REGISTRY, registry,
                                                  G_TYPE_STRING, sender);
    }

  return DEX_FUTURE (g_steal_pointer (&promise));
}

DexFuture *
xdp_app_info_registry_insert_future (XdpAppInfoRegistry    *registry,
                                     GDBusMethodInvocation *invocation,
                                     DexFuture             *app_info_future)
{
  g_autoptr(QueueData) data = NULL;
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  G_MUTEX_AUTO_LOCK (&registry->channels_lock, locker);

  data = g_new0 (QueueData, 1);
  data->kind = QUEUE_DATA_KIND_INSERT;
  data->invocation = g_object_ref (invocation);
  data->app_info_future = g_steal_pointer (&app_info_future);

  return queue_work (registry, sender, g_steal_pointer (&data));
}

DexFuture *
xdp_app_info_registry_delete_future (XdpAppInfoRegistry *registry,
                                     const char         *sender)
{
  g_autoptr(QueueData) data = NULL;

  G_MUTEX_AUTO_LOCK (&registry->channels_lock, locker);

  /* Shortcut if we have no active channel (i.e. no in-flight changes) */
  if (!g_hash_table_contains (registry->channels, sender))
    {
      return dex_future_new_for_boolean (g_hash_table_remove (registry->app_infos,
                                                              sender));
    }

  data = g_new0 (QueueData, 1);
  data->kind = QUEUE_DATA_KIND_DELETE;
  data->sender = g_strdup (sender);

  return queue_work (registry, sender, g_steal_pointer (&data));
}

DexFuture *
xdp_app_info_registry_ensure_future (XdpAppInfoRegistry    *registry,
                                     GDBusMethodInvocation *invocation)
{
  g_autoptr(QueueData) data = NULL;
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  XdpAppInfo *existing = g_hash_table_lookup (registry->app_infos, sender);

  G_MUTEX_AUTO_LOCK (&registry->channels_lock, locker);

  /* Shortcut if we have no active channel (i.e. no in-flight changes) */
  if (existing && !g_hash_table_contains (registry->channels, sender))
    return dex_future_new_take_object (g_object_ref (existing));

  data = g_new0 (QueueData, 1);
  data->kind = QUEUE_DATA_KIND_ENSURE;
  data->invocation = g_object_ref (invocation);

  return queue_work (registry, sender, g_steal_pointer (&data));
}
