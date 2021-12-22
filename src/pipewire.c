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

#include <errno.h>
#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "pipewire.h"

#define ROUNDTRIP_TIMEOUT_SECS 10

typedef struct _PipeWireSource
{
  GSource base;

  PipeWireRemote *remote;
} PipeWireSource;

static gboolean is_pipewire_initialized = FALSE;

static void
registry_event_global (void *user_data,
                       uint32_t id,
                       uint32_t permissions,
                       const char *type,
                       uint32_t version,
                       const struct spa_dict *props)
{
  PipeWireRemote *remote = user_data;
  const struct spa_dict_item *factory_object_type;
  PipeWireGlobal *global;

  global = g_new0 (PipeWireGlobal, 1);
  *global = (PipeWireGlobal) {
    .parent_id = id,
  };

  g_hash_table_insert (remote->globals, GINT_TO_POINTER (id), global);
  if (remote->global_added_cb)
    remote->global_added_cb (remote, id, type, props, remote->user_data);

  if (strcmp(type, PW_TYPE_INTERFACE_Factory) != 0)
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

static void
registry_event_global_remove (void *user_data,
                              uint32_t id)
{
  PipeWireRemote *remote = user_data;

  if (remote->global_removed_cb)
    remote->global_removed_cb (remote, id, remote->user_data);
  g_hash_table_remove (remote->globals, GINT_TO_POINTER (id));
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = registry_event_global,
  .global_remove = registry_event_global_remove,
};

static void
on_roundtrip_timeout (void *user_data,
                      uint64_t expirations)
{
  PipeWireRemote *remote = user_data;

  g_warning ("PipeWire roundtrip timed out waiting for events");
  pw_main_loop_quit (remote->loop);
}

void
pipewire_remote_roundtrip (PipeWireRemote *remote)
{
  struct timespec roundtrip_timeout_spec = { ROUNDTRIP_TIMEOUT_SECS, 0 };

  remote->sync_seq = pw_core_sync (remote->core, PW_ID_CORE, remote->sync_seq);

  /* Arm the roundtrip timeout before running the main loop, then clear it
     right afterwards. */
  pw_loop_update_timer (pw_main_loop_get_loop (remote->loop),
                        remote->roundtrip_timeout,
                        &roundtrip_timeout_spec,
                        NULL,
                        FALSE);

  pw_main_loop_run (remote->loop);

  pw_loop_update_timer (pw_main_loop_get_loop (remote->loop),
                        remote->roundtrip_timeout,
                        NULL,
                        NULL,
                        FALSE);
}

static gboolean
discover_node_factory_sync (PipeWireRemote *remote,
                            GError **error)
{
  struct pw_registry *registry;

  registry = pw_core_get_registry (remote->core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener (registry,
                            &remote->registry_listener,
                            &registry_events,
                            remote);

  pipewire_remote_roundtrip (remote);

  pw_proxy_destroy((struct pw_proxy*)registry);

  if (remote->node_factory_id == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No node factory discovered");
      return FALSE;
    }

  return TRUE;
}

static void
core_event_error (void       *user_data,
                  uint32_t    id,
		  int         seq,
		  int         res,
		  const char *message)
{
  PipeWireRemote *remote = user_data;

  if (id == PW_ID_CORE)
    {
      g_set_error (&remote->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "%s", message);
      pw_main_loop_quit (remote->loop);
    }
}

static void
core_event_done (void *user_data,
                 uint32_t id, int seq)
{
  PipeWireRemote *remote = user_data;

  if (id == PW_ID_CORE && remote->sync_seq == seq)
    pw_main_loop_quit (remote->loop);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = core_event_error,
  .done = core_event_done,
};

static gboolean
pipewire_loop_source_prepare (GSource *base,
                              int *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean
pipewire_loop_source_dispatch (GSource *source,
                               GSourceFunc callback,
                               gpointer user_data)
{
  PipeWireSource *pipewire_source = (PipeWireSource *) source;
  struct pw_loop *loop;
  int result;

  loop = pw_main_loop_get_loop (pipewire_source->remote->loop);
  result = pw_loop_iterate (loop, 0);
  if (result < 0)
    g_warning ("pipewire_loop_iterate failed: %s", spa_strerror (result));

  if (pipewire_source->remote->error)
    {
      GFunc error_callback;

      g_warning ("Caught PipeWire error: %s", pipewire_source->remote->error->message);

      error_callback = pipewire_source->remote->error_callback;
      if (error_callback)
        error_callback (pipewire_source->remote,
                        pipewire_source->remote->user_data);
    }

  return TRUE;
}

static void
pipewire_loop_source_finalize (GSource *source)
{
  PipeWireSource *pipewire_source = (PipeWireSource *) source;
  struct pw_loop *loop;

  loop = pw_main_loop_get_loop (pipewire_source->remote->loop);
  pw_loop_leave (loop);
}

static GSourceFuncs pipewire_source_funcs =
{
  pipewire_loop_source_prepare,
  NULL,
  pipewire_loop_source_dispatch,
  pipewire_loop_source_finalize
};

void
pipewire_remote_destroy (PipeWireRemote *remote)
{
  if (remote->roundtrip_timeout != NULL)
    {
      struct pw_loop *loop = pw_main_loop_get_loop (remote->loop);
      pw_loop_destroy_source (loop, g_steal_pointer (&remote->roundtrip_timeout));
    }

  g_clear_pointer (&remote->globals, g_hash_table_destroy);
  g_clear_pointer (&remote->core, pw_core_disconnect);
  g_clear_pointer (&remote->context, pw_context_destroy);
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

GSource *
pipewire_remote_create_source (PipeWireRemote *remote)
{
  PipeWireSource *pipewire_source;
  struct pw_loop *loop;


  pipewire_source = (PipeWireSource *) g_source_new (&pipewire_source_funcs,
                                                     sizeof (PipeWireSource));
  pipewire_source->remote = remote;

  loop = pw_main_loop_get_loop (pipewire_source->remote->loop);
  g_source_add_unix_fd (&pipewire_source->base,
                        pw_loop_get_fd (loop),
                        G_IO_IN | G_IO_ERR);

  pw_loop_enter (loop);
  g_source_attach (&pipewire_source->base, NULL);

  return &pipewire_source->base;
}

PipeWireRemote *
pipewire_remote_new_sync (struct pw_properties *pipewire_properties,
                          PipeWireGlobalAddedCallback global_added_cb,
                          PipeWireGlobalRemovedCallback global_removed_cb,
                          GFunc error_callback,
                          gpointer user_data,
                          GError **error)
{
  PipeWireRemote *remote;

  ensure_pipewire_is_initialized ();

  remote = g_new0 (PipeWireRemote, 1);

  remote->global_added_cb = global_added_cb;
  remote->global_removed_cb = global_removed_cb;
  remote->error_callback = error_callback;
  remote->user_data = user_data;

  remote->loop = pw_main_loop_new (NULL);
  if (!remote->loop)
    {
      pipewire_remote_destroy (remote);
      pw_properties_free (pipewire_properties);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire main loop");
      return NULL;
    }

  remote->context = pw_context_new (pw_main_loop_get_loop (remote->loop), NULL, 0);
  if (!remote->context)
    {
      pipewire_remote_destroy (remote);
      pw_properties_free (pipewire_properties);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire context");
      return NULL;
    }

  remote->core = pw_context_connect (remote->context, pipewire_properties, 0);
  if (!remote->core)
    {
      pipewire_remote_destroy (remote);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't connect to PipeWire");
      return NULL;
    }

  remote->roundtrip_timeout = pw_loop_add_timer (pw_main_loop_get_loop (remote->loop),
                                                 on_roundtrip_timeout,
                                                 remote);

  remote->globals = g_hash_table_new_full (NULL, NULL, NULL, g_free);

  pw_core_add_listener (remote->core,
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
