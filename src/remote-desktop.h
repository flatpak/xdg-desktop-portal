/*
 * Copyright © 2017-2018 Red Hat, Inc
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
 *
 */

#pragma once

#include <gio/gio.h>

#include "xdp-session.h"
#include "xdp-types.h"

typedef struct _RemoteDesktopSession RemoteDesktopSession;

GType remote_desktop_session_get_type (void);

G_GNUC_UNUSED static inline RemoteDesktopSession *
REMOTE_DESKTOP_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, remote_desktop_session_get_type (), RemoteDesktopSession);
}

G_GNUC_UNUSED static inline gboolean
IS_REMOTE_DESKTOP_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, remote_desktop_session_get_type ());
}

GList * remote_desktop_session_get_streams (RemoteDesktopSession *session);

gboolean remote_desktop_session_can_select_sources (RemoteDesktopSession *session);

gboolean remote_desktop_session_can_request_clipboard (RemoteDesktopSession *session);

gboolean remote_desktop_session_is_clipboard_enabled (RemoteDesktopSession *session);

void remote_desktop_session_sources_selected (RemoteDesktopSession *session);

void remote_desktop_session_clipboard_requested (RemoteDesktopSession *session);

void init_remote_desktop (XdpContext *context);
