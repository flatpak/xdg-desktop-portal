/*
 * Copyright © 2026 Red Hat, Inc
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

#include <gobject/gvaluecollector.h>

#include "xdp-dex.h"

typedef struct _DexSchedulerSpawnTrampoline
{
  GCallback callback;
  GArray *values;
} DexSchedulerSpawnTrampoline;

static void
dex_scheduler_spawn_trampoline_free (DexSchedulerSpawnTrampoline *state)
{
  state->callback = NULL;
  g_clear_pointer (&state->values, g_array_unref);
  g_free (state);
}

static inline DexFuture *
dex_scheduler_spawn_trampoline_fiber (gpointer data)
{
  DexSchedulerSpawnTrampoline *state = data;
  GClosure *closure = NULL;
  GValue return_value = G_VALUE_INIT;
  gpointer res;

  g_assert (state != NULL);
  g_assert (state->callback != NULL);
  g_assert (state->values != NULL);

  g_value_init (&return_value, G_TYPE_POINTER);
  closure = g_cclosure_new (state->callback, NULL, NULL);
  g_closure_set_marshal (closure, g_cclosure_marshal_generic);
  g_closure_invoke (closure,
                    &return_value,
                    state->values->len,
                    (const GValue *)(gpointer)state->values->data,
                    NULL);
  res = g_value_get_pointer (&return_value);

  g_closure_unref (closure);
  g_value_unset (&return_value);

  return res;
}

/**
 * dex_scheduler_spawn_closure: (skip)
 * @scheduler: (nullable): a [class@Dex.Scheduler]
 * @stack_size: stack size in bytes or 0
 * @callback: the fiber to spawn
 * @n_params: number of arguments of the fiber
 * @...: arguments, pairs of #GType followed by the value
 *
 * Same as dex_scheduler_spawn() but trampolines into a fiber without having to
 * create special structures on the way there.
 *
 * ```c
 * static DexFuture *
 * fiber_func (GInputStream *stream,
 *             int           num)
 * {
 *   ...
 *
 *   return dex_future_new_true ();
 * }
 *
 * DexFuture *
 * spawn_fiber (GInputStream *stream)
 * {
 *   return dex_scheduler_spawn_closure (NULL, 0,
 *                                       G_CALLBACK (fiber_func),
 *                                       2,
 *                                       G_TYPE_POINTER, stream,
 *                                       G_TYPE_INT, 42);
 * }
 * ```
 *
 * Returns: (transfer full): a [class@Dex.Future] that will resolve or reject when
 *   @callback completes (or its resulting `DexFuture` completes).
 */
DexFuture *
dex_scheduler_spawn_closure (DexScheduler *scheduler,
                             gsize         stack_size,
                             GCallback     callback,
                             guint         n_params,
                             ...)
{
  char *errmsg = NULL;
  GArray *values = NULL;
  DexSchedulerSpawnTrampoline *state;
  DexFuture *future;
  va_list args;

  values = g_array_new (FALSE, TRUE, sizeof (GValue));
  g_array_set_clear_func (values, (GDestroyNotify)g_value_unset);
  g_array_set_size (values, n_params);

  va_start (args, n_params);

  for (guint i = 0; i < n_params; i++)
    {
      GType gtype = va_arg (args, GType);
      GValue *dest = &g_array_index (values, GValue, i);
      GValue value = G_VALUE_INIT;

      G_VALUE_COLLECT_INIT (&value, gtype, args, 0, &errmsg);

      if (errmsg != NULL)
        break;

      g_value_init (dest, gtype);
      g_value_copy (&value, dest);
      g_value_unset (&value);
    }

  va_end (args);

  if (errmsg != NULL)
    {
      future = dex_future_new_reject (DEX_ERROR,
                                      DEX_ERROR_TYPE_MISMATCH,
                                      "Failed to trampoline to fiber: %s",
                                      errmsg);
    }
  else
    {
      state = g_new0 (DexSchedulerSpawnTrampoline, 1);
      state->values = g_steal_pointer (&values);
      state->callback = callback;

      future = dex_scheduler_spawn (scheduler, stack_size,
                                    dex_scheduler_spawn_trampoline_fiber,
                                    state,
                                    (GDestroyNotify) dex_scheduler_spawn_trampoline_free);
    }

  g_free (errmsg);
  if (values)
    g_array_unref (values);

  return future;
}
