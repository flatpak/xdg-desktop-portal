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

typedef struct _PipeWireRemote
{
  struct pw_main_loop *loop;
  struct pw_core *core;
  struct pw_remote *remote;
  struct spa_hook remote_listener;

  struct pw_core_proxy *core_proxy;
  struct spa_hook core_listener;
  uint32_t sync_seq;

  struct spa_hook registry_listener;

  uint32_t node_factory_id;

  GError *error;
} PipeWireRemote;

PipeWireRemote * pipewire_remote_new_sync (struct pw_properties *pipewire_properties,
                                           GError **error);

void pipewire_remote_destroy (PipeWireRemote *remote);

void pipewire_remote_roundtrip (PipeWireRemote *remote);
