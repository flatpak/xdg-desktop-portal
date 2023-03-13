/*
 * Copyright © 2023 Red Hat
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

#pragma once

#include "session.h"

typedef enum _PersistMode
{
  PERSIST_MODE_NONE = 0,
  PERSIST_MODE_TRANSIENT = 1,
  PERSIST_MODE_PERSISTENT = 2,
} PersistMode;

void set_transient_permissions (const char *sender,
                                const char *restore_token,
                                GVariant *restore_data);

void delete_transient_permissions (const char *sender,
                                   const char *restore_token);

GVariant * get_transient_permissions (const char *sender,
                                      const char *restore_token);

void set_persistent_permissions (const char *table,
                                 const char *app_id,
                                 const char *restore_token,
                                 GVariant *restore_data);

void delete_persistent_permissions (const char *table,
                                    const char *app_id,
                                    const char *restore_token);

GVariant * get_persistent_permissions (const char *table,
                                       const char *app_id,
                                       const char *restore_token);

void remove_transient_permissions_for_sender (const char *sender);

void replace_restore_token_with_data (Session *session,
                                      const char *table,
                                      GVariant **in_out_options,
                                      char **out_restore_token);

void replace_restore_data_with_token (Session *session,
                                      const char *table,
                                      GVariant **in_out_results,
                                      PersistMode *in_out_persist_mode,
                                      char **in_out_restore_token,
                                      GVariant **in_out_restore_data);

void generate_and_save_restore_token (Session *session,
                                      const char *table,
                                      PersistMode persist_mode,
                                      char **in_out_restore_token,
                                      GVariant **in_out_restore_data);
