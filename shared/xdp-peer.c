/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-peer-private.h"

#include <glib/gstdio.h>

XdpPeerCredentials *
xdp_peer_credentials_ref (XdpPeerCredentials *self)
{
  g_atomic_ref_count_inc (&self->rc);
  return self;
}

void
xdp_peer_credentials_unref (XdpPeerCredentials *self)
{
  if (!g_atomic_ref_count_dec (&self->rc))
    return;
  g_clear_fd (&self->fd, NULL);
  g_free (self);
}

G_DEFINE_BOXED_TYPE (XdpPeerCredentials, xdp_peer_credentials,
                     xdp_peer_credentials_ref, xdp_peer_credentials_unref);

XdpPeerCredentials *
xdp_peer_credentials_new (uint32_t pid,
                          int      fd)
{
  XdpPeerCredentials *credentials = g_new0 (XdpPeerCredentials, 1);

  g_atomic_ref_count_init (&credentials->rc);
  credentials->pid = pid;
  credentials->fd = fd;

  return credentials;
}

typedef struct _XdpPeerPrivate
{
  /* Identifies the connection for its lifetime, and is never reused */
  char *key;
} XdpPeerPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (XdpPeer, xdp_peer, G_TYPE_OBJECT);

typedef enum
{
  PROP_KEY = 1,
} XdpPeerProps;

static GParamSpec *properties [PROP_KEY + 1];

static void
xdp_peer_dispose (GObject *object)
{
  XdpPeerPrivate *priv = xdp_peer_get_instance_private (XDP_PEER (object));

  g_clear_pointer (&priv->key, g_free);

  G_OBJECT_CLASS (xdp_peer_parent_class)->dispose (object);
}

static void
xdp_peer_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  XdpPeerPrivate *priv = xdp_peer_get_instance_private (XDP_PEER (object));

  switch ((XdpPeerProps) prop_id)
    {
    case PROP_KEY:
      g_value_set_string (value, priv->key);
      break;
    }
}

static void
xdp_peer_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  XdpPeerPrivate *priv = xdp_peer_get_instance_private (XDP_PEER (object));

  switch ((XdpPeerProps) prop_id)
    {
    case PROP_KEY:
      g_assert (priv->key == NULL);
      priv->key = g_value_dup_string (value);
      break;
    }
}

static void
xdp_peer_class_init (XdpPeerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_peer_dispose;
  object_class->get_property = xdp_peer_get_property;
  object_class->set_property = xdp_peer_set_property;

  properties[PROP_KEY] =
    g_param_spec_string ("key", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

static void
xdp_peer_init (XdpPeer *peer)
{
}

const char *
xdp_peer_get_key (XdpPeer *peer)
{
  XdpPeerPrivate *priv;

  g_return_val_if_fail (XDP_IS_PEER (peer), NULL);

  priv = xdp_peer_get_instance_private (peer);

  return priv->key;
}

DexFuture *
xdp_peer_resolve_credentials (XdpPeer *peer)
{
  dex_return_error_if_fail (XDP_IS_PEER (peer));

  return XDP_PEER_GET_CLASS (peer)->resolve_credentials (peer);
}
