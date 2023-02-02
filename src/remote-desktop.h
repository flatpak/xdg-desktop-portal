/*
 * Copyright © 2017-2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "session.h"
#include "screen-cast.h"

typedef struct _RemoteDesktopSession RemoteDesktopSession;

gboolean is_remote_desktop_session (Session *session);

GList * remote_desktop_session_get_streams (RemoteDesktopSession *session);

gboolean remote_desktop_session_can_select_sources (RemoteDesktopSession *session);


void remote_desktop_session_sources_selected (RemoteDesktopSession *session);

GDBusInterfaceSkeleton * remote_desktop_create (GDBusConnection *connection,
                                                const char      *dbus_name);
