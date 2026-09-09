/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "xdp-types.h"

#if HAVE_VARLINK
#include "xdp-varlink.h"
#endif

void init_registry (XdpContext *context);

#if HAVE_VARLINK
gboolean init_registry_varlink (XdpVarlinkService  *service,
                                GError            **error);
#endif
