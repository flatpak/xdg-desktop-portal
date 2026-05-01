/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <stdint.h>

#include <gio/gio.h>
#include <pipewire/pipewire.h>

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

  struct pw_registry *registry;
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
