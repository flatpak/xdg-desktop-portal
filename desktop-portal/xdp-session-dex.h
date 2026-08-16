/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-dbus.h"
#include "xdp-types.h"

#define XDP_TYPE_SESSION_DEX (xdp_session_dex_get_type ())
G_DECLARE_FINAL_TYPE (XdpSessionDex,
                      xdp_session_dex,
                      XDP, SESSION_DEX,
                      XdpDbusSessionSkeleton)

DexFuture * xdp_session_dex_new (XdpContext             *context,
                                 XdpAppInfo             *app_info,
                                 GDBusInterfaceSkeleton *skeleton,
                                 GDBusProxy             *proxy_impl,
                                 GVariant               *arg_options);

gboolean xdp_session_dex_is_closed (XdpSessionDex *session);

void xdp_session_dex_close (XdpSessionDex *session);

XdpAppInfo * xdp_session_dex_get_app_info (XdpSessionDex *session);

const char * xdp_session_dex_get_object_path (XdpSessionDex *session);

#define XDP_TYPE_SESSION_DEX_STORE (xdp_session_dex_store_get_type ())
G_DECLARE_FINAL_TYPE (XdpSessionDexStore,
                      xdp_session_dex_store,
                      XDP, SESSION_DEX_STORE,
                      GObject)

XdpSessionDexStore * xdp_session_dex_store_new (void);

XdpSessionDexStore * xdp_session_dex_store_new_with_offset (size_t session_offset);

#define xdp_session_dex_store_new_wrapped(MySessionWrapper, session) \
  xdp_session_dex_store_new_with_offset (G_STRUCT_OFFSET (MySessionWrapper, session))

gpointer xdp_session_dex_store_lookup_session (XdpSessionDexStore *store,
                                               const char         *session_handle,
                                               XdpAppInfo         *app_info);

void xdp_session_dex_store_take_session (XdpSessionDexStore *store,
                                         gpointer            session_wrapper);
