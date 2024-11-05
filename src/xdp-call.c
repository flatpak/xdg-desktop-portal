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

#include "xdp-call.h"

static void
xdp_call_free (XdpCall *call)
{
  g_clear_object (&call->app_info);
  g_clear_pointer (&call->sender, g_free);
  g_free (call);
}

void
xdp_call_init_invocation (GDBusMethodInvocation *invocation,
                          XdpAppInfo            *app_info)
{
  XdpCall *call;

  call = g_new0 (XdpCall, 1);
  call->app_info = g_object_ref (app_info);
  call->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  g_object_set_data_full (G_OBJECT (invocation), "xdp-call",
                          call, (GDestroyNotify) xdp_call_free);
}

XdpCall *
xdp_call_from_invocation (GDBusMethodInvocation *invocation)
{
  return g_object_get_data (G_OBJECT (invocation), "xdp-call");
}
