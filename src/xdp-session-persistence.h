/*
 * Copyright © 2023 Red Hat
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

#include "session.h"

typedef enum _XdpSessionPersistenceMode
{
  XDP_SESSION_PERSISTENCE_MODE_NONE = 0,
  XDP_SESSION_PERSISTENCE_MODE_TRANSIENT = 1,
  XDP_SESSION_PERSISTENCE_MODE_PERSISTENT = 2,
} XdpSessionPersistenceMode;

void xdp_session_persistence_set_transient_permissions (XdpSession *session,
                                                        const char *restore_token,
                                                        GVariant *restore_data);

void xdp_session_persistence_delete_transient_permissions (XdpSession *session,
                                                           const char *restore_token);

void xdp_session_persistence_delete_transient_permissions_for_sender (const char *sender_name);

GVariant * xdp_session_persistence_get_transient_permissions (XdpSession *session,
                                                              const char *restore_token);

void xdp_session_persistence_set_persistent_permissions (XdpSession *session,
                                                         const char *table,
                                                         const char *restore_token,
                                                         GVariant *restore_data);

void xdp_session_persistence_delete_persistent_permissions (XdpSession *session,
                                                            const char *table,
                                                            const char *restore_token);

GVariant * xdp_session_persistence_get_persistent_permissions (XdpSession *session,
                                                               const char *table,
                                                               const char *restore_token);

void xdp_session_persistence_replace_restore_token_with_data (XdpSession *session,
                                                              const char *table,
                                                              GVariant **in_out_options,
                                                              char **out_restore_token);

void xdp_session_persistence_replace_restore_data_with_token (XdpSession *session,
                                                              const char *table,
                                                              GVariant **in_out_results,
                                                              XdpSessionPersistenceMode *in_out_persist_mode,
                                                              char **in_out_restore_token,
                                                              GVariant **in_out_restore_data);

void xdp_session_persistence_generate_and_save_restore_token (XdpSession *session,
                                                              const char *table,
                                                              XdpSessionPersistenceMode persist_mode,
                                                              char **in_out_restore_token,
                                                              GVariant **in_out_restore_data);
