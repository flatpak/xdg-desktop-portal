/*
 * Copyright © 2018 Red Hat, Inc
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

#include <gio/gio.h>
#include "xdp-app-info.h"
#include "xdp-utils.h"

typedef struct _Call
{
  XdpAppInfo *app_info;
  char *sender;
} Call;

void call_init_invocation (GDBusMethodInvocation *invocation,
                           XdpAppInfo *app_info);

Call *call_from_invocation (GDBusMethodInvocation *invocation);
