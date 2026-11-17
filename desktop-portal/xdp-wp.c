/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-wp.h"

static void
xdp_wp_core_sync_cb (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (DexPromise) promise = DEX_PROMISE (user_data);
  g_autoptr (GError) error = NULL;

  if (wp_core_sync_finish (WP_CORE (object), result, &error))
    dex_promise_resolve_boolean (promise, TRUE);
  else
    dex_promise_reject (promise, g_steal_pointer (&error));
}

DexFuture *
xdp_wp_core_sync (WpCore *core)
{
  DexPromise *promise = dex_promise_new_cancellable ();

  g_assert (WP_IS_CORE (core));

  wp_core_sync (core,
                dex_promise_get_cancellable (promise),
                xdp_wp_core_sync_cb,
                dex_ref (promise));

  return DEX_FUTURE (promise);
}

static DexFuture *
xdp_wp_core_connect_sync_then (DexFuture *future,
                               gpointer   user_data)
{
  WpCore *core = WP_CORE (user_data);

  if (wp_core_is_connected (core))
    return dex_future_new_true ();

  return dex_future_new_for_error (g_error_new_literal (G_IO_ERROR,
                                                        G_IO_ERROR_FAILED,
                                                        "Connection failed"));
}

DexFuture *
xdp_wp_core_connect_sync (WpCore *core)
{
  g_assert (WP_IS_CORE (core));

  wp_core_connect (core);

  return dex_future_then (xdp_wp_core_sync (core),
                          xdp_wp_core_connect_sync_then,
                          g_object_ref (core),
                          g_object_unref);
}
