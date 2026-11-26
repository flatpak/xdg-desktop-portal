/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#pragma once

#include <wp/wp.h>

#include "xdp-impl-dbus.h"

#define XDP_WP_TYPE_APP_AUDIO_CAPTURE (xdp_wp_app_audio_capture_get_type())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioCapture, xdp_wp_app_audio_capture, XDP_WP, APP_AUDIO_CAPTURE,
                      XdpDbusImplAppAudioCaptureSkeleton);

XdpWpAppAudioCapture * xdp_wp_app_audio_capture_new (WpCore          *core,
                                                     GDBusConnection *connection);
