/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-peer-dbus-private.h"

#include <glib/gstdio.h>

#include "xdp-dex.h"
#include "xdp-types.h"
#include "xdp-utils.h"

struct _XdpPeerDbus
{
  XdpPeer parent;

  GDBusConnection *connection;
};

G_DEFINE_FINAL_TYPE (XdpPeerDbus, xdp_peer_dbus, XDP_TYPE_PEER);

typedef enum
{
  PROP_CONNECTION = 1,
} XdpPeerDbusProps;

static GParamSpec *properties [PROP_CONNECTION + 1];

static XdpPeerCredentials *
connection_get_pidfd_legacy_fiber (GDBusConnection *connection,
                                   const char      *sender,
                                   GError         **error)
{
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) reply = NULL;
  uint32_t pid;

  future = dex_dbus_connection_call (connection,
                                     DBUS_DBUS_NAME,
                                     DBUS_DBUS_PATH,
                                     DBUS_DBUS_IFACE,
                                     "GetConnectionUnixProcessID",
                                     g_variant_new ("(s)", sender),
                                     G_VARIANT_TYPE ("(u)"),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     30000);

  reply = dex_await_variant (g_steal_pointer (&future), error);
  if (!reply)
    return NULL;

  g_variant_get (reply, "(u)", &pid);
  return xdp_peer_credentials_new (pid, -1);
}

static DexFuture *
connection_get_pidfd_fiber (GDBusConnection *connection,
                            const char      *sender)
{
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) dict = NULL;
  g_autoptr(GVariant) process_fd = NULL;
  g_autoptr(GVariant) process_id = NULL;
  int fd_id;
  uint32_t pid;
  g_autofd int pidfd = -1;

  future = dex_dbus_connection_call_with_unix_fd_list (connection,
                                                       DBUS_DBUS_NAME,
                                                       DBUS_DBUS_PATH,
                                                       DBUS_DBUS_IFACE,
                                                       "GetConnectionCredentials",
                                                       g_variant_new ("(s)", sender),
                                                       G_VARIANT_TYPE ("(a{sv})"),
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       30000,
                                                       NULL);

  if (!dex_await (dex_ref (future), &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE))
        {
          g_autoptr(XdpPeerCredentials) credentials = NULL;

          g_clear_error (&local_error);
          credentials = connection_get_pidfd_legacy_fiber (connection, sender, &local_error);

          if (credentials)
            return dex_future_new_take_boxed (XDP_TYPE_PEER_CREDENTIALS,
                                              g_steal_pointer (&credentials));
        }
      return dex_future_new_for_error (g_steal_pointer (&local_error));
    }

  reply = dex_await_variant (dex_ref (dex_future_set_get_future_at (DEX_FUTURE_SET (future), 0)), NULL);
  fd_list = dex_await_object (dex_ref (dex_future_set_get_future_at (DEX_FUTURE_SET (future), 1)), NULL);

  g_variant_get (reply, "(@a{sv})", &dict);

  process_id = g_variant_lookup_value (dict, "ProcessID", G_VARIANT_TYPE_UINT32);
  if (!process_id)
    {
      g_autoptr(XdpPeerCredentials) credentials = NULL;

      credentials = connection_get_pidfd_legacy_fiber (connection, sender, &local_error);

      if (credentials)
        return dex_future_new_take_boxed (XDP_TYPE_PEER_CREDENTIALS,
                                          g_steal_pointer (&credentials));
      else
        return dex_future_new_for_error (g_steal_pointer (&local_error));
    }

  pid = g_variant_get_uint32 (process_id);

  process_fd = g_variant_lookup_value (dict, "ProcessFD", G_VARIANT_TYPE_HANDLE);
  if (!process_fd)
    return dex_future_new_take_boxed (XDP_TYPE_PEER_CREDENTIALS,
                                      xdp_peer_credentials_new (pid, -1));

  fd_id = g_variant_get_handle (process_fd);

  if (fd_list == NULL)
    return dex_future_new_reject (G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer pidfd");

  if (!xdp_is_fd_list_index_valid (fd_list, fd_id))
    return dex_future_new_reject (G_IO_ERROR, G_IO_ERROR_FAILED, "Pidfd index is out of bounds");

  pidfd = g_unix_fd_list_get (fd_list, fd_id, &local_error);
  if (pidfd < 0)
    return dex_future_new_for_error (g_steal_pointer (&local_error));

  return dex_future_new_take_boxed (XDP_TYPE_PEER_CREDENTIALS,
                                    xdp_peer_credentials_new (pid, g_steal_fd (&pidfd)));
}

static DexFuture *
xdp_peer_dbus_resolve_credentials (XdpPeer *peer)
{
  XdpPeerDbus *peer_dbus = XDP_PEER_DBUS (peer);
  const char *sender = xdp_peer_get_key (peer);

  dex_return_error_if_fail (G_IS_DBUS_CONNECTION (peer_dbus->connection));
  dex_return_error_if_fail (sender != NULL);

  return dex_scheduler_spawnv (NULL, 0,
                               G_CALLBACK (connection_get_pidfd_fiber),
                               2,
                               G_TYPE_DBUS_CONNECTION, peer_dbus->connection,
                               G_TYPE_STRING, sender);
}

static void
xdp_peer_dbus_dispose (GObject *object)
{
  XdpPeerDbus *peer_dbus = XDP_PEER_DBUS (object);

  g_clear_object (&peer_dbus->connection);

  G_OBJECT_CLASS (xdp_peer_dbus_parent_class)->dispose (object);
}

static void
xdp_peer_dbus_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  XdpPeerDbus *peer_dbus = XDP_PEER_DBUS (object);

  switch ((XdpPeerDbusProps) prop_id)
    {
    case PROP_CONNECTION:
      g_value_set_object (value, peer_dbus->connection);
      break;
    }
}

static void
xdp_peer_dbus_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  XdpPeerDbus *peer_dbus = XDP_PEER_DBUS (object);

  switch ((XdpPeerDbusProps) prop_id)
    {
    case PROP_CONNECTION:
      g_assert (peer_dbus->connection == NULL);
      peer_dbus->connection = g_value_dup_object (value);
      break;
    }
}

static void
xdp_peer_dbus_class_init (XdpPeerDbusClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpPeerClass *peer_class = XDP_PEER_CLASS (klass);

  object_class->dispose = xdp_peer_dbus_dispose;
  object_class->get_property = xdp_peer_dbus_get_property;
  object_class->set_property = xdp_peer_dbus_set_property;

  peer_class->resolve_credentials = xdp_peer_dbus_resolve_credentials;

  properties[PROP_CONNECTION] =
    g_param_spec_object ("connection", NULL, NULL,
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

static void
xdp_peer_dbus_init (XdpPeerDbus *peer_dbus)
{
}

XdpPeer *
xdp_peer_dbus_new (GDBusConnection *connection,
                   const char      *sender)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  g_return_val_if_fail (sender != NULL, NULL);

  return g_object_new (XDP_TYPE_PEER_DBUS,
                       "connection", connection,
                       "key", sender,
                       NULL);
}
