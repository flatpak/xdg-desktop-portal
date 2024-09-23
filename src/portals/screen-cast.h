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
#include <stdint.h>

typedef struct _ScreenCastStream ScreenCastStream;

uint32_t screen_cast_stream_get_pipewire_node_id (ScreenCastStream *stream);

void screen_cast_stream_get_size (ScreenCastStream *stream,
                                  int32_t *width,
                                  int32_t *height);

void screen_cast_stream_free (ScreenCastStream *stream);

void screen_cast_stream_get_size (ScreenCastStream *stream,
                                  int32_t *width,
                                  int32_t *height);

void screen_cast_remove_transient_permissions_for_sender (const char *sender);

GList * collect_screen_cast_stream_data (GVariantIter *streams_iter);

GDBusInterfaceSkeleton * screen_cast_create (GDBusConnection *connection,
                                             const char      *dbus_name);
