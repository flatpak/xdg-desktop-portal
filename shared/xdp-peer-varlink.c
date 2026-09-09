/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-peer-varlink-private.h"

#include <sys/socket.h>

#include "xdp-utils.h"

/* Linux 6.5, may be missing from the build headers */
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

struct _XdpPeerVarlink
{
  XdpPeer parent;

  /* Read while the socket was known live; the descriptor may not be */
  XdpPeerCredentials *credentials;
};

G_DEFINE_FINAL_TYPE (XdpPeerVarlink, xdp_peer_varlink, XDP_TYPE_PEER);

static XdpPeerCredentials *
socket_get_credentials (int socket_fd)
{
  struct ucred ucred;
  socklen_t len = sizeof (ucred);
  g_autofd int pidfd = -1;
  int peer_pidfd;
  socklen_t pidfd_len = sizeof (peer_pidfd);

  if (getsockopt (socket_fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0)
    return NULL;

  if (getsockopt (socket_fd,
                  SOL_SOCKET,
                  SO_PEERPIDFD,
                  &peer_pidfd,
                  &pidfd_len) == 0)
    pidfd = peer_pidfd;

  return xdp_peer_credentials_new (ucred.pid, g_steal_fd (&pidfd));
}

static DexFuture *
xdp_peer_varlink_resolve_credentials (XdpPeer *peer)
{
  XdpPeerVarlink *peer_varlink = XDP_PEER_VARLINK (peer);

  if (peer_varlink->credentials == NULL)
    return dex_future_new_reject (G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "Can't read varlink peer credentials");

  return dex_future_new_take_boxed (XDP_TYPE_PEER_CREDENTIALS,
                                    xdp_peer_credentials_ref (peer_varlink
                                                                ->credentials));
}

static void
xdp_peer_varlink_dispose (GObject *object)
{
  XdpPeerVarlink *peer_varlink = XDP_PEER_VARLINK (object);

  g_clear_pointer (&peer_varlink->credentials, xdp_peer_credentials_unref);

  G_OBJECT_CLASS (xdp_peer_varlink_parent_class)->dispose (object);
}

static void
xdp_peer_varlink_class_init (XdpPeerVarlinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpPeerClass *peer_class = XDP_PEER_CLASS (klass);

  object_class->dispose = xdp_peer_varlink_dispose;

  peer_class->resolve_credentials = xdp_peer_varlink_resolve_credentials;
}

static void
xdp_peer_varlink_init (XdpPeerVarlink *peer_varlink)
{
}

XdpPeer *
xdp_peer_varlink_new (int         socket_fd,
                      const char *key)
{
  XdpPeerVarlink *peer_varlink;

  g_return_val_if_fail (socket_fd >= 0, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  peer_varlink = g_object_new (XDP_TYPE_PEER_VARLINK, "key", key, NULL);
  peer_varlink->credentials = socket_get_credentials (socket_fd);

  return XDP_PEER (peer_varlink);
}
