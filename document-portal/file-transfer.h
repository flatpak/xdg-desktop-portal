/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>

GDBusInterfaceSkeleton *file_transfer_create (void);

void stop_file_transfers_for_sender (const char *name);
