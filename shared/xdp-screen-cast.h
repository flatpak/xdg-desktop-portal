/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

typedef enum _XdpScreenCastSourceType
{
  XDP_SCREEN_CAST_SOURCE_TYPE_MONITOR = 1 << 0,
  XDP_SCREEN_CAST_SOURCE_TYPE_WINDOW = 1 << 1,
  XDP_SCREEN_CAST_SOURCE_TYPE_VIRTUAL = 1 << 2,
} XdpScreenCastSourceType;

typedef enum _XdpScreenCastAudioType
{
  XDP_SCREEN_CAST_AUDIO_TYPE_NONE = 1 << 0,
  XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM = 1 << 1,
  XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION = 1 << 2,
} XdpScreenCastAudioType;
