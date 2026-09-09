/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-varlink-session.h"

#include "xdp-varlink.h"

static GQuark quark_parked_session;

typedef struct
{
  /* NULL once there is nothing left to reply to */
  VarlinkCall *call;
} ParkedSession;

static void
parked_session_free (gpointer data)
{
  ParkedSession *parked = data;

  g_clear_pointer (&parked->call, varlink_call_unref);
  g_free (parked);
}

static ParkedSession *
parked_session_get (XdpSessionDex *session)
{
  return g_object_get_qdata (G_OBJECT (session), quark_parked_session);
}

/* The final reply is what tells the client that the session ended */
static void
on_session_closed (XdpSessionDex *session,
                   gpointer       user_data)
{
  ParkedSession *parked = user_data;
  g_autoptr(VarlinkObject) reply = NULL;

  if (parked->call == NULL)
    return;

  varlink_object_new (&reply);
  xdp_varlink_call_reply (parked->call, reply, 0);

  g_clear_pointer (&parked->call, varlink_call_unref);
}

static void
on_connection_closed (XdpVarlinkConnection *connection,
                      gpointer              user_data)
{
  XdpSessionDex *session = XDP_SESSION_DEX (user_data);
  ParkedSession *parked = parked_session_get (session);

  /* Nothing to reply to, so the close below stays silent */
  g_clear_pointer (&parked->call, varlink_call_unref);

  xdp_session_dex_close (session, FALSE);
}

int64_t
xdp_varlink_session_next_handle (void)
{
  static int64_t last_handle;

  return ++last_handle;
}

void
xdp_varlink_session_park (XdpSessionDex        *session,
                          XdpVarlinkConnection *connection,
                          VarlinkCall          *call)
{
  ParkedSession *parked;

  g_return_if_fail (XDP_IS_SESSION_DEX (session));
  g_return_if_fail (XDP_IS_VARLINK_CONNECTION (connection));
  g_return_if_fail (call != NULL);

  quark_parked_session = g_quark_from_static_string ("xdp-parked-session");

  parked = g_new0 (ParkedSession, 1);
  parked->call = varlink_call_ref (call);

  g_object_set_qdata_full (G_OBJECT (session), quark_parked_session,
                           parked, parked_session_free);

  g_signal_connect (session, "session-closed",
                    G_CALLBACK (on_session_closed),
                    parked);

  g_signal_connect_object (connection, "closed",
                           G_CALLBACK (on_connection_closed),
                           session,
                           G_CONNECT_DEFAULT);
}
