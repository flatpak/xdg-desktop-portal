/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-varlink-connection.h"

#include <sys/socket.h>

#include <libdex.h>

#include "xdp-peer-varlink-private.h"

/* Linux 4.13, may be missing from the build headers */
#ifndef SO_COOKIE
#define SO_COOKIE 57
#endif

struct _XdpVarlinkConnection
{
  GObject parent_instance;

  guint64 cookie;
  XdpAppInfoRegistry *app_info_registry;
  XdpPeer *peer;

  /* Owned; the last connection to drop an instance disposes it */
  XdpVarlinkInstance *instance;
};

G_DEFINE_FINAL_TYPE (XdpVarlinkConnection, xdp_varlink_connection, G_TYPE_OBJECT);

enum
{
  CLOSED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void
xdp_varlink_connection_dispose (GObject *object)
{
  XdpVarlinkConnection *self = XDP_VARLINK_CONNECTION (object);

  g_signal_emit (self, signals[CLOSED], 0);

  if (self->instance != NULL)
    {
      dex_future_disown (xdp_app_info_registry_delete_future (
        self->app_info_registry, xdp_peer_get_key (self->peer)));
    }

  g_clear_object (&self->instance);
  g_clear_object (&self->app_info_registry);
  g_clear_object (&self->peer);

  G_OBJECT_CLASS (xdp_varlink_connection_parent_class)->dispose (object);
}

static void
xdp_varlink_connection_class_init (XdpVarlinkConnectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_varlink_connection_dispose;

  /* Portals watch this rather than libvarlink's per-call closed callback,
   * which has a single slot the dispatcher owns */
  signals[CLOSED] = g_signal_new ("closed",
                                  XDP_TYPE_VARLINK_CONNECTION,
                                  G_SIGNAL_RUN_LAST,
                                  0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
xdp_varlink_connection_init (XdpVarlinkConnection *self)
{
}

gboolean
xdp_varlink_socket_cookie (int      socket_fd,
                           guint64 *cookie_out)
{
  socklen_t len = sizeof (*cookie_out);

  return getsockopt (socket_fd, SOL_SOCKET, SO_COOKIE, cookie_out, &len) == 0;
}

XdpVarlinkConnection *
xdp_varlink_connection_new (XdpAppInfoRegistry *app_info_registry,
                            int                 socket_fd,
                            guint64             cookie)
{
  XdpVarlinkConnection *self;
  g_autofree char *key = NULL;

  g_return_val_if_fail (XDP_IS_APP_INFO_REGISTRY (app_info_registry), NULL);
  g_return_val_if_fail (socket_fd >= 0, NULL);

  key = g_strdup_printf ("varlink:%" G_GUINT64_FORMAT, cookie);

  self = g_object_new (XDP_TYPE_VARLINK_CONNECTION, NULL);
  self->app_info_registry = g_object_ref (app_info_registry);
  self->cookie = cookie;
  self->peer = xdp_peer_varlink_new (socket_fd, key);

  return self;
}

XdpPeer *
xdp_varlink_connection_get_peer (XdpVarlinkConnection *self)
{
  g_return_val_if_fail (XDP_IS_VARLINK_CONNECTION (self), NULL);

  return self->peer;
}

XdpVarlinkInstance *
xdp_varlink_connection_get_instance (XdpVarlinkConnection *self)
{
  g_return_val_if_fail (XDP_IS_VARLINK_CONNECTION (self), NULL);

  return self->instance;
}

XdpAppInfo *
xdp_varlink_connection_get_app_info (XdpVarlinkConnection *self)
{
  g_return_val_if_fail (XDP_IS_VARLINK_CONNECTION (self), NULL);

  if (self->instance == NULL)
    return NULL;

  return xdp_varlink_instance_get_app_info (self->instance);
}

void
xdp_varlink_connection_claim (XdpVarlinkConnection *self,
                              XdpVarlinkInstance *instance)
{
  g_return_if_fail (XDP_IS_VARLINK_CONNECTION (self));
  g_return_if_fail (XDP_IS_VARLINK_INSTANCE (instance));
  g_return_if_fail (self->instance == NULL);

  self->instance = g_object_ref (instance);
}
