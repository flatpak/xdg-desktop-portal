/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "call.h"

static void
call_free (Call *call)
{
  g_clear_object (&call->app_info);
  g_clear_pointer (&call->sender, g_free);
  g_free (call);
}

void
call_init_invocation (GDBusMethodInvocation *invocation,
                      XdpAppInfo *app_info)
{
  Call *call;

  call = g_new0 (Call, 1);
  call->app_info = g_object_ref (app_info);
  call->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  g_object_set_data_full (G_OBJECT (invocation), "call",
                          call, (GDestroyNotify) call_free);
}

Call *
call_from_invocation (GDBusMethodInvocation *invocation)
{
  return g_object_get_data (G_OBJECT (invocation), "call");
}
