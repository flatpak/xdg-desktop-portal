/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <libdex.h>

#include "xdp-types.h"

#if HAVE_VARLINK
#include "xdp-varlink.h"
#endif

DexFuture * init_settings (gpointer user_data);

#if HAVE_VARLINK
gboolean init_settings_varlink (XdpVarlinkService  *service,
                                XdpContext         *context,
                                GError            **error);
#endif
