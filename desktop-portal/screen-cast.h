/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <stdint.h>

#include <gio/gio.h>

#include "xdp-types.h"

typedef struct _ScreenCastStream ScreenCastStream;

uint32_t screen_cast_stream_get_pipewire_node_id (ScreenCastStream *stream);

void screen_cast_stream_get_size (ScreenCastStream *stream,
                                  int32_t *width,
                                  int32_t *height);

void screen_cast_stream_free (ScreenCastStream *stream);

void screen_cast_stream_get_size (ScreenCastStream *stream,
                                  int32_t *width,
                                  int32_t *height);

GList * collect_screen_cast_stream_data (GVariantIter *streams_iter);

void init_screen_cast (XdpContext *context);
