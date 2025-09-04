/*
 * Copyright © 2022 Red Hat, Inc
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

#include "xdp-types.h"

GType input_capture_session_get_type (void);

typedef struct _InputCaptureSession InputCaptureSession;

G_GNUC_UNUSED static inline InputCaptureSession *
INPUT_CAPTURE_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, input_capture_session_get_type (), InputCaptureSession);
}

G_GNUC_UNUSED static inline gboolean
IS_INPUT_CAPTURE_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, input_capture_session_get_type ());
}

gboolean input_capture_session_can_request_clipboard (InputCaptureSession *session);

gboolean input_capture_session_is_clipboard_enabled (InputCaptureSession *session);

void input_capture_session_clipboard_requested (InputCaptureSession *session);

gboolean input_capture_session_can_access_clipboard (InputCaptureSession *session);

void init_input_capture (XdpContext *context);
