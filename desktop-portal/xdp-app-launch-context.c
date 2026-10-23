/* SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "xdp-app-launch-context.h"

static char **original_environment;

void
xdp_app_launch_context_init_environment (void)
{
  g_return_if_fail (original_environment == NULL);

  original_environment = g_get_environ ();
}

struct _XdpAppLaunchContext
{
  GAppLaunchContext parent_instance;

  char *token;
};

G_DEFINE_FINAL_TYPE (XdpAppLaunchContext,
                     xdp_app_launch_context,
                     G_TYPE_APP_LAUNCH_CONTEXT);

void
xdp_app_launch_context_set_activation_token (XdpAppLaunchContext *self,
                                             const char          *token)
{
  g_set_str (&self->token, token);
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
  const char * const variables[] = { "GIO_USE_VFS", "GTK_USE_PORTAL" };
  GAppLaunchContext *context = G_APP_LAUNCH_CONTEXT (self);

  g_return_if_fail (original_environment != NULL);

  for (size_t i = 0; i < G_N_ELEMENTS (variables); i++)
    {
      const char *value = g_environ_getenv (original_environment, variables[i]);

      if (value != NULL)
        g_app_launch_context_setenv (context, variables[i], value);
      else
        g_app_launch_context_unsetenv (context, variables[i]);
    }
}

XdpAppLaunchContext *
xdp_app_launch_context_new (void)
{
  return g_object_new (XDP_TYPE_APP_LAUNCH_CONTEXT, NULL);
}
