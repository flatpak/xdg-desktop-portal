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

#include "xdp-queue-future.h"

struct _XdpQueueFuture
{
  GObject parent_instance;

  DexPromise *pending_promise;
};

G_DEFINE_FINAL_TYPE (XdpQueueFuture,
                     xdp_queue_future,
                     G_TYPE_OBJECT)

struct _XdpQueueFutureGuard
{
  GObject parent_instance;

  DexPromise *promise;
};

G_DEFINE_FINAL_TYPE (XdpQueueFutureGuard,
                     xdp_queue_future_guard,
                     G_TYPE_OBJECT)

static void
xdp_queue_future_dispose (GObject *object)
{
  XdpQueueFuture *queue_future = XDP_QUEUE_FUTURE (object);

  g_clear_pointer (&queue_future->pending_promise, dex_unref);

  G_OBJECT_CLASS (xdp_queue_future_parent_class)->dispose (object);
}

static void
xdp_queue_future_class_init (XdpQueueFutureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_queue_future_dispose;
}

static void
xdp_queue_future_init (XdpQueueFuture *queue_future)
{
}

XdpQueueFuture *
xdp_queue_future_new (void)
{
  return g_object_new (XDP_TYPE_QUEUE_FUTURE, NULL);
}

static DexFuture *
on_queue_future_dependency_resolved (DexFuture *future,
                                     gpointer   user_data)
{
  XdpQueueFutureGuard *next_guard = XDP_QUEUE_FUTURE_GUARD (user_data);

  return dex_future_new_for_object (next_guard);
}

DexFuture *
xdp_queue_future_next (XdpQueueFuture *queue_future)
{
  g_autoptr(DexPromise) pending_promise = NULL;
  g_autoptr(XdpQueueFutureGuard) next_guard = NULL;

  next_guard = g_object_new (XDP_TYPE_QUEUE_FUTURE_GUARD, NULL);
  pending_promise = g_steal_pointer (&queue_future->pending_promise);
  queue_future->pending_promise = dex_ref (next_guard->promise);

  if (!pending_promise || dex_future_is_resolved (DEX_FUTURE (pending_promise)))
    return dex_future_new_take_object (g_steal_pointer (&next_guard));

  return dex_future_finally (DEX_FUTURE (g_steal_pointer (&pending_promise)),
                             on_queue_future_dependency_resolved,
                             g_steal_pointer (&next_guard),
                             g_object_unref);
}

static void
xdp_queue_future_guard_dispose (GObject *object)
{
  XdpQueueFutureGuard *guard = XDP_QUEUE_FUTURE_GUARD (object);

  g_warning ("xdp_queue_future_guard_dispose, resolving promise");
  if (guard->promise)
    {
      dex_promise_resolve_boolean (guard->promise, TRUE);
      g_clear_pointer (&guard->promise, dex_unref);
    }

  G_OBJECT_CLASS (xdp_queue_future_guard_parent_class)->dispose (object);
}

static void
xdp_queue_future_guard_class_init (XdpQueueFutureGuardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_queue_future_guard_dispose;
}

static void
xdp_queue_future_guard_init (XdpQueueFutureGuard *guard)
{
  guard->promise = dex_promise_new ();
}
