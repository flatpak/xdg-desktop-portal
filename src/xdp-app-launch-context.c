/* launch-context.c
 *
 * Copyright 2024 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "xdp-app-launch-context.h"

struct _XdpAppLaunchContext
{
  GAppLaunchContext parent_instance;

  char *token;
};

G_DEFINE_TYPE (XdpAppLaunchContext,
               xdp_app_launch_context,
               G_TYPE_APP_LAUNCH_CONTEXT)

void
xdp_app_launch_context_set_activation_token (XdpAppLaunchContext *self,
                                             const char          *token)
{
  g_clear_pointer (&self->token, g_free);
  self->token = g_strdup (token);
}

static char *
xdp_app_launch_context_get_startup_notify_id (GAppLaunchContext *context,
                                              GAppInfo          *info,
                                              GList             *files)
{
  XdpAppLaunchContext *self = XDP_APP_LAUNCH_CONTEXT (context);

  return g_strdup (self->token);
}

static void
xdp_app_launch_context_finalize (GObject *object)
{
  XdpAppLaunchContext *self = XDP_APP_LAUNCH_CONTEXT (object);

  g_clear_pointer (&self->token, g_free);

  G_OBJECT_CLASS (xdp_app_launch_context_parent_class)->finalize (object);
}

static void
xdp_app_launch_context_class_init (XdpAppLaunchContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GAppLaunchContextClass *g_klass = G_APP_LAUNCH_CONTEXT_CLASS (klass);

  object_class->finalize = xdp_app_launch_context_finalize;
  g_klass->get_startup_notify_id = xdp_app_launch_context_get_startup_notify_id;
}

static void
xdp_app_launch_context_init (XdpAppLaunchContext *self)
{
}

XdpAppLaunchContext *
xdp_app_launch_context_new (void)
{
  return g_object_new (XDP_TYPE_APP_LAUNCH_CONTEXT, NULL);
}
