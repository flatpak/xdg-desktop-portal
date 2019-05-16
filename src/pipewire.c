/*
 * Copyright © 2017-2019 Red Hat, Inc
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

#include "config.h"

#include <glib.h>
#include <pipewire/pipewire.h>

#include "pipewire.h"

static gboolean is_pipewire_initialized = FALSE;

static void
registry_event_global (void *user_data,
                       uint32_t id,
                       uint32_t parent_id,
                       uint32_t permissions,
                       uint32_t type,
                       uint32_t version,
                       const struct spa_dict *props)
{
  PipeWireRemote *remote = user_data;
  struct pw_type *core_type = pw_core_get_type (remote->core);
  const struct spa_dict_item *factory_object_type;
  PipeWireGlobal *global;

  global = g_new0 (PipeWireGlobal, 1);
  *global = (PipeWireGlobal) {
    .parent_id = parent_id,
  };

  g_hash_table_insert (remote->globals, GINT_TO_POINTER (id), global);

  if (type != core_type->factory)
    return;

  factory_object_type = spa_dict_lookup_item (props, "factory.type.name");
  if (!factory_object_type)
    return;

  if (strcmp (factory_object_type->value, "PipeWire:Interface:ClientNode") == 0)
    {
      remote->node_factory_id = id;
      pw_main_loop_quit (remote->loop);
    }
}

static const struct pw_registry_proxy_events registry_events = {
  PW_VERSION_REGISTRY_PROXY_EVENTS,
  .global = registry_event_global,
};

void
pipewire_remote_roundtrip (PipeWireRemote *remote)
{
  pw_core_proxy_sync (remote->core_proxy, ++remote->sync_seq);
  pw_main_loop_run (remote->loop);
}

static gboolean
discover_node_factory_sync (PipeWireRemote *remote,
                            GError **error)
{
  struct pw_type *core_type = pw_core_get_type (remote->core);
  struct pw_registry_proxy *registry_proxy;

  registry_proxy = pw_core_proxy_get_registry (remote->core_proxy,
                                               core_type->registry,
                                               PW_VERSION_REGISTRY, 0);
  pw_registry_proxy_add_listener (registry_proxy,
                                  &remote->registry_listener,
                                  &registry_events,
                                  remote);

  pipewire_remote_roundtrip (remote);

  if (remote->node_factory_id == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No node factory discovered");
      return FALSE;
    }

  return TRUE;
}

static void
on_state_changed (void *user_data,
                  enum pw_remote_state old,
                  enum pw_remote_state state,
                  const char *error)
{
  PipeWireRemote *remote = user_data;

  switch (state)
    {
    case PW_REMOTE_STATE_ERROR:
      g_set_error (&remote->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", error);
      pw_main_loop_quit (remote->loop);
      break;
    case PW_REMOTE_STATE_UNCONNECTED:
      g_set_error (&remote->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Disconnected");
      pw_main_loop_quit (remote->loop);
      break;
    case PW_REMOTE_STATE_CONNECTING:
      break;
    case PW_REMOTE_STATE_CONNECTED:
      pw_main_loop_quit (remote->loop);
      break;
    default:
      g_warning ("Unknown PipeWire state");
      break;
    }
}

static const struct pw_remote_events remote_events = {
  PW_VERSION_REMOTE_EVENTS,
  .state_changed = on_state_changed,
};

static void
core_event_done (void *user_data,
                 uint32_t seq)
{
  PipeWireRemote *remote = user_data;

  if (remote->sync_seq == seq)
    pw_main_loop_quit (remote->loop);
}

static const struct pw_core_proxy_events core_events = {
  PW_VERSION_CORE_PROXY_EVENTS,
  .done = core_event_done,
};

void
pipewire_remote_destroy (PipeWireRemote *remote)
{
  g_clear_pointer (&remote->globals, g_hash_table_destroy);
  g_clear_pointer (&remote->remote, pw_remote_destroy);
  g_clear_pointer (&remote->core, pw_core_destroy);
  g_clear_pointer (&remote->loop, pw_main_loop_destroy);
  g_clear_error (&remote->error);

  g_free (remote);
}

static void
ensure_pipewire_is_initialized (void)
{
  if (is_pipewire_initialized)
    return;

  pw_init (NULL, NULL);

  is_pipewire_initialized = TRUE;
}

PipeWireRemote *
pipewire_remote_new_sync (struct pw_properties *pipewire_properties,
                          GError **error)
{
  PipeWireRemote *remote;

  ensure_pipewire_is_initialized ();

  remote = g_new0 (PipeWireRemote, 1);

  remote->loop = pw_main_loop_new (NULL);
  if (!remote->loop)
    {
      pipewire_remote_destroy (remote);
      pw_properties_free (pipewire_properties);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire main loop");
      return NULL;
    }

  remote->core = pw_core_new (pw_main_loop_get_loop (remote->loop), NULL);
  if (!remote->core)
    {
      pipewire_remote_destroy (remote);
      pw_properties_free (pipewire_properties);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire core");
      return NULL;
    }

  remote->remote = pw_remote_new (remote->core, pipewire_properties, 0);
  if (!remote->remote)
    {
      pipewire_remote_destroy (remote);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire remote");
      return NULL;
    }

  remote->globals = g_hash_table_new_full (NULL, NULL, NULL, g_free);

  pw_remote_add_listener (remote->remote,
                          &remote->remote_listener,
                          &remote_events,
                          remote);

  if (pw_remote_connect (remote->remote) != 0)
    {
      pipewire_remote_destroy (remote);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't connect PipeWire remote");
      return NULL;
    }

  pw_main_loop_run (remote->loop);

  switch (pw_remote_get_state (remote->remote, NULL))
    {
    case PW_REMOTE_STATE_ERROR:
    case PW_REMOTE_STATE_UNCONNECTED:
      *error = g_steal_pointer (&remote->error);
      pipewire_remote_destroy (remote);
      return NULL;
    case PW_REMOTE_STATE_CONNECTING:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "PipeWire loop stopped unexpectedly");
      pipewire_remote_destroy (remote);
      return NULL;
    case PW_REMOTE_STATE_CONNECTED:
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unexpected PipeWire state");
      pipewire_remote_destroy (remote);
      return NULL;
    }

  remote->core_proxy = pw_remote_get_core_proxy (remote->remote);
  pw_core_proxy_add_listener (remote->core_proxy,
                              &remote->core_listener,
                              &core_events,
                              remote);

  if (!discover_node_factory_sync (remote, error))
    {
      pipewire_remote_destroy (remote);
      return NULL;
    }

  return remote;
}
