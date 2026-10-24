/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>
#include <libdex.h>

#define XDP_TYPE_IMPL_OWNER_WATCH (xdp_impl_owner_watch_get_type ())
G_DECLARE_FINAL_TYPE (XdpImplOwnerWatch,
                      xdp_impl_owner_watch,
                      XDP, IMPL_OWNER_WATCH,
                      GObject)

typedef void (*XdpImplOwnerLostFunc) (XdpImplOwnerWatch *watch,
                                    GObject           *object);

/* The callback holds a reference to object, but the watch does not. It runs in
 * the subscribing thread's main context and must not block on a request/session
 * lock held by a portal handler. */
XdpImplOwnerWatch * xdp_impl_owner_watch_new (GDBusProxy           *proxy,
                                              GObject              *object,
                                              XdpImplOwnerLostFunc   owner_lost);

gboolean xdp_impl_owner_watch_is_current (XdpImplOwnerWatch *watch);

/* Close is only sent to the captured unique owner, never the well-known name.
 * A backend that has already disappeared or changed needs no cleanup. */
void xdp_impl_owner_watch_close (XdpImplOwnerWatch *watch,
                                 GDBusProxy        *impl_proxy);

gboolean xdp_impl_owner_watch_close_sync (XdpImplOwnerWatch  *watch,
                                           GDBusProxy         *impl_proxy,
                                           GError            **error);

DexFuture * xdp_impl_owner_watch_close_future (XdpImplOwnerWatch *watch,
                                               GDBusProxy        *impl_proxy);
