/*
 * Copyright © 2017-2018 Red Hat, Inc
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

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <stdint.h>

typedef struct _PipeWireRemote PipeWireRemote;

typedef struct _PipeWireGlobal
{
  uint32_t parent_id;
  gboolean permission_set;
} PipeWireGlobal;

typedef void (* PipeWireGlobalAddedCallback) (PipeWireRemote *remote,
                                              uint32_t id,
                                              const char *type,
                                              const struct spa_dict *props,
                                              gpointer user_data);

typedef void (* PipeWireGlobalRemovedCallback) (PipeWireRemote *remote,
                                                uint32_t id,
                                                gpointer user_data);

struct _PipeWireRemote
{
  struct pw_main_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;

  struct spa_source *roundtrip_timeout;

  int sync_seq;

  struct spa_hook registry_listener;

  GHashTable *globals;
  PipeWireGlobalAddedCallback global_added_cb;
  PipeWireGlobalRemovedCallback global_removed_cb;
  gpointer user_data;
  GFunc error_callback;

  uint32_t node_factory_id;

  GError *error;
};

PipeWireRemote * pipewire_remote_new_sync (struct pw_properties *pipewire_properties,
                                           PipeWireGlobalAddedCallback global_added_cb,
                                           PipeWireGlobalRemovedCallback global_removed_cb,
                                           GFunc error_callback,
                                           gpointer user_data,
                                           GError **error);

void pipewire_remote_destroy (PipeWireRemote *remote);

void pipewire_remote_roundtrip (PipeWireRemote *remote);

GSource * pipewire_remote_create_source (PipeWireRemote *remote);
