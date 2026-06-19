/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <glib-object.h>
#include <libdex.h>

#if !HAVE_DEX_SCHEDULER_SPAWNV
DexFuture *
dex_scheduler_spawnv (DexScheduler *scheduler,
                      gsize         stack_size,
                      GCallback     callback,
                      guint         n_params,
                      ...);
#endif /* !HAVE_DEX_SCHEDULER_SPAWNV */
