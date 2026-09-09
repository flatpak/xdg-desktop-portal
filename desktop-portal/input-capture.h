/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-app-info.h"
#include "xdp-types.h"

#if HAVE_VARLINK
#include "xdp-varlink.h"
#endif

#define XDP_TYPE_INPUT_CAPTURE_SESSION (xdp_input_capture_session_get_type ())
G_DECLARE_FINAL_TYPE (XdpInputCaptureSession,
                      xdp_input_capture_session,
                      XDP, INPUT_CAPTURE_SESSION,
                      GObject)

XdpInputCaptureSession * input_capture_lookup_session (XdpContext *context,
                                                       const char *session_handle,
                                                       XdpAppInfo *app_info);

gboolean input_capture_session_can_request_clipboard (XdpInputCaptureSession *session);

gboolean input_capture_session_is_clipboard_enabled (XdpInputCaptureSession *session);

void input_capture_session_clipboard_requested (XdpInputCaptureSession *session);

gboolean input_capture_session_can_access_clipboard (XdpInputCaptureSession *session);

const char * input_capture_session_get_object_path (XdpInputCaptureSession *session);

XdpAppInfo * input_capture_session_get_app_info (XdpInputCaptureSession *session);

gboolean input_capture_session_is_closed (XdpInputCaptureSession *session);

void init_input_capture (XdpContext *context);

#if HAVE_VARLINK
gboolean init_input_capture_varlink (XdpVarlinkService  *service,
                                     XdpContext         *context,
                                     GError            **error);
#endif
