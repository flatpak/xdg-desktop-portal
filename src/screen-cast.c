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

#include "config.h"

#include <stdint.h>
#include <pipewire/pipewire.h>
#include <gio/gunixfdlist.h>

#include "session.h"
#include "screen-cast.h"
#include "remote-desktop.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _PipeWireRemote
{
  struct pw_main_loop *loop;
  struct pw_core *core;
  struct pw_remote *remote;
  struct spa_hook remote_listener;

  uint32_t registry_sync_seq;
  uint32_t node_factory_id;

  GError *error;
} PipeWireRemote;

typedef struct _ScreenCast ScreenCast;
typedef struct _ScreenCastClass ScreenCastClass;

struct _ScreenCast
{
  XdpScreenCastSkeleton parent_instance;
};

struct _ScreenCastClass
{
  XdpScreenCastSkeletonClass parent_class;
};

static XdpImplScreenCast *impl;
static ScreenCast *screen_cast;
static gboolean is_pipewire_initialized = FALSE;

GType screen_cast_get_type (void);
static void screen_cast_iface_init (XdpScreenCastIface *iface);

static GQuark quark_request_session;

struct _ScreenCastStream
{
  uint32_t id;
  int32_t width;
  int32_t height;
};

G_DEFINE_TYPE_WITH_CODE (ScreenCast, screen_cast, XDP_TYPE_SCREEN_CAST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SCREEN_CAST,
                                                screen_cast_iface_init))

typedef enum _ScreenCastSessionState
{
  SCREEN_CAST_SESSION_STATE_INIT,
  SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES,
  SCREEN_CAST_SESSION_STATE_SOURCES_SELECTED,
  SCREEN_CAST_SESSION_STATE_STARTING,
  SCREEN_CAST_SESSION_STATE_STARTED,
  SCREEN_CAST_SESSION_STATE_CLOSED
} ScreenCastSessionState;

typedef struct _ScreenCastSession
{
  Session parent;

  ScreenCastSessionState state;

  GList *streams;
} ScreenCastSession;

typedef struct _ScreenCastSessionClass
{
  SessionClass parent_class;
} ScreenCastSessionClass;

GType screen_cast_session_get_type (void);

G_DEFINE_TYPE (ScreenCastSession, screen_cast_session, session_get_type ())

static gboolean
is_screen_cast_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, screen_cast_session_get_type ());
}

static ScreenCastSession *
screen_cast_session_new (GVariant *options,
                         Request *request,
                         GError **error)
{
  Session *session;
  GDBusInterfaceSkeleton *interface_skeleton =
    G_DBUS_INTERFACE_SKELETON (request);
  const char *session_token;
  GDBusConnection *connection =
    g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (screen_cast_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  g_debug ("screen cast session owned by '%s' created",
           session->sender);

  return (ScreenCastSession*)session;
}

static void
create_session_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  guint response = 2;
  gboolean should_close_session;
  g_autofree char *session_id = NULL;
  GVariantBuilder results_builder;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_impl_screen_cast_call_create_session_finish (impl,
                                                        &response,
                                                        NULL,
                                                        res,
                                                        &error))
    {
      g_warning ("A backend call failed: %s", error->message);
      should_close_session = TRUE;
      goto out;
    }

  if (request->exported && response == 0)
    {
      if (!session_export (session, &error))
        {
          g_warning ("Failed to export session: %s", error->message);
          response = 2;
          should_close_session = TRUE;
          goto out;
        }

      should_close_session = FALSE;
      session_register (session);
    }
  else
    {
      should_close_session = TRUE;
    }

  g_variant_builder_add (&results_builder, "{sv}",
                         "session_handle", g_variant_new ("s", session->id));

out:
  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results_builder));
      request_unexport (request);
    }
  else
    {
      g_variant_builder_clear (&results_builder);
    }

  if (should_close_session)
    session_close (session, FALSE);
}

static gboolean
handle_create_session (XdpScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;

  REQUEST_AUTOLOCK (request);

  impl_request =
    xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                     G_DBUS_PROXY_FLAGS_NONE,
                                     g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                     request->id,
                                     NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = (Session *)screen_cast_session_new (arg_options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_impl_screen_cast_call_create_session (impl,
                                            request->id,
                                            session->id,
                                            xdp_app_info_get_id (request->app_info),
                                            options,
                                            NULL,
                                            create_session_done,
                                            g_object_ref (request));

  xdp_screen_cast_complete_create_session (object, invocation, request->id);

  return TRUE;
}

static void
select_sources_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  guint response = 2;
  gboolean should_close_session;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) results = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_impl_screen_cast_call_select_sources_finish (impl,
                                                        &response,
                                                        &results,
                                                        res,
                                                        &error))
    g_warning ("A backend call failed: %s", error->message);

  should_close_session = !request->exported || response != 0;

  if (request->exported)
    {
      if (!results)
        {
          GVariantBuilder results_builder;

          g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_request_emit_response (XDP_REQUEST (request), response, results);
      request_unexport (request);
    }

  if (should_close_session)
    {
      session_close (session, TRUE);
    }
  else if (!session->closed)
    {
      if (is_screen_cast_session (session))
        {
          ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

          g_assert_cmpint (screen_cast_session->state,
                           ==,
                           SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES);
          screen_cast_session->state = SCREEN_CAST_SESSION_STATE_SOURCES_SELECTED;
        }
      else if (is_remote_desktop_session (session))
        {
          RemoteDesktopSession *remote_desktop_session =
            (RemoteDesktopSession *)session;

          remote_desktop_session_sources_selected (remote_desktop_session);
        }
    }
}

static XdpOptionKey screen_cast_select_sources_options[] = {
  { "types", G_VARIANT_TYPE_UINT32 },
  { "multiple", G_VARIANT_TYPE_BOOLEAN },
};

static gboolean
handle_select_sources (XdpScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options_builder;

  REQUEST_AUTOLOCK (request);

  session = acquire_session (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (is_screen_cast_session (session))
    {
      ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

      switch (screen_cast_session->state)
        {
        case SCREEN_CAST_SESSION_STATE_INIT:
          break;
        case SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES:
        case SCREEN_CAST_SESSION_STATE_SOURCES_SELECTED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Sources already selected");
          return TRUE;
        case SCREEN_CAST_SESSION_STATE_STARTING:
        case SCREEN_CAST_SESSION_STATE_STARTED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Can only select sources before starting");
          return TRUE;
        case SCREEN_CAST_SESSION_STATE_CLOSED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Invalid session");
          return TRUE;
        }
    }
  else if (is_remote_desktop_session (session))
    {
      RemoteDesktopSession *remote_desktop_session =
        (RemoteDesktopSession *)session;

      if (!remote_desktop_session_can_select_sources (remote_desktop_session))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Invalid state");
          return TRUE;
        }
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return TRUE;
    }

  impl_request =
    xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                     G_DBUS_PROXY_FLAGS_NONE,
                                     g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                     request->id,
                                     NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &options_builder,
                      screen_cast_select_sources_options,
                      G_N_ELEMENTS (screen_cast_select_sources_options));

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);
  if (is_screen_cast_session (session))
    {
      ((ScreenCastSession *)session)->state =
        SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES;
    }
  else
    {
      remote_desktop_session_selecting_sources ((RemoteDesktopSession *)session);
    }

  xdp_impl_screen_cast_call_select_sources (impl,
                                            request->id,
                                            arg_session_handle,
                                            xdp_app_info_get_id (request->app_info),
                                            g_variant_builder_end (&options_builder),
                                            NULL,
                                            select_sources_done,
                                            g_object_ref (request));

  xdp_screen_cast_complete_select_sources (object, invocation, request->id);

  return TRUE;
}

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

static void
core_event_done (void *user_data,
                 uint32_t seq)
{
  PipeWireRemote *remote = user_data;

  if (remote->registry_sync_seq == seq)
    pw_main_loop_quit (remote->loop);
}

static const struct pw_core_proxy_events core_events = {
  PW_VERSION_CORE_PROXY_EVENTS,
  .done = core_event_done,
};

static gboolean
discover_node_factory_sync (PipeWireRemote *remote,
                            GError **error)
{
  struct pw_type *core_type = pw_core_get_type (remote->core);
  struct pw_core_proxy *core_proxy;
  struct spa_hook core_listener;
  struct pw_registry_proxy *registry_proxy;
  struct spa_hook registry_listener;

  core_proxy = pw_remote_get_core_proxy (remote->remote);
  pw_core_proxy_add_listener (core_proxy,
                              &core_listener,
                              &core_events,
                              remote);

  registry_proxy = pw_core_proxy_get_registry (core_proxy,
                                               core_type->registry,
                                               PW_VERSION_REGISTRY, 0);
  pw_registry_proxy_add_listener (registry_proxy,
                                  &registry_listener,
                                  &registry_events,
                                  remote);

  pw_main_loop_run (remote->loop);

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

void
pipewire_remote_destroy (PipeWireRemote *remote)
{
  g_clear_pointer (&remote->remote, (GDestroyNotify)pw_remote_destroy);
  g_clear_pointer (&remote->core, (GDestroyNotify)pw_core_destroy);
  g_clear_pointer (&remote->loop, (GDestroyNotify)pw_main_loop_destroy);
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

static PipeWireRemote *
connect_pipewire_sync (GError **error)
{
  PipeWireRemote *remote;

  ensure_pipewire_is_initialized ();

  remote = g_new0 (PipeWireRemote, 1);

  remote->loop = pw_main_loop_new (NULL);
  if (!remote->loop)
    {
      pipewire_remote_destroy (remote);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire main loop");
      return NULL;
    }

  remote->core = pw_core_new (pw_main_loop_get_loop (remote->loop), NULL);
  if (!remote->core)
    {
      pipewire_remote_destroy (remote);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire core");
      return NULL;
    }

  remote->remote = pw_remote_new (remote->core, NULL, 0);
  if (!remote->remote)
    {
      pipewire_remote_destroy (remote);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create PipeWire remote");
      return NULL;
    }

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
      return FALSE;
    case PW_REMOTE_STATE_CONNECTING:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "PipeWire loop stopped unexpectedly");
      pipewire_remote_destroy (remote);
      return FALSE;
    case PW_REMOTE_STATE_CONNECTED:
      return remote;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unexpected PipeWire state");
      pipewire_remote_destroy (remote);
      return FALSE;
    }
}

uint32_t
screen_cast_stream_get_pipewire_node_id (ScreenCastStream *stream)
{
  return stream->id;
}

static PipeWireRemote *
open_pipewire_screen_cast_remote (GList *streams,
                                  GError **error)
{
  PipeWireRemote *remote;
  GList *l;
  unsigned int n_streams, i;
  struct spa_dict_item *permission_items;
  unsigned int n_permission_items;
  g_autofree char *node_factory_permission_string = NULL;
  char **stream_permission_values;

  remote = connect_pipewire_sync (error);
  if (!remote)
    return FALSE;

  if (!discover_node_factory_sync (remote, error))
    {
      pipewire_remote_destroy (remote);
      return NULL;
    }

  n_streams = g_list_length (streams);
  n_permission_items = n_streams + 4;
  permission_items = g_new0 (struct spa_dict_item, n_permission_items);

  /*
   * Hide all existing and future nodes (except the ones we explicitly list below.
   */
  permission_items[0].key = PW_CORE_PROXY_PERMISSIONS_EXISTING;
  permission_items[0].value = "---";
  permission_items[1].key = PW_CORE_PROXY_PERMISSIONS_DEFAULT;
  permission_items[1].value = "---";

  /*
   * PipeWire:Interface:Core
   * Needs rwx to be able create the sink node using the create-object method
   */
  permission_items[2].key = PW_CORE_PROXY_PERMISSIONS_GLOBAL;
  permission_items[2].value = "0:rwx";

  /*
   * PipeWire:Interface:NodeFactory
   * Needs r-- so it can be passed to create-object when creating the sink node.
   */
  node_factory_permission_string = g_strdup_printf ("%d:r--",
                                                    remote->node_factory_id);
  permission_items[3].key = PW_CORE_PROXY_PERMISSIONS_GLOBAL;
  permission_items[3].value = node_factory_permission_string;

  i = 4;
  stream_permission_values = g_new0 (char *, n_streams + 1);
  for (l = streams; l; l = l->next)
    {
      ScreenCastStream *stream = l->data;
      uint32_t stream_id;
      char *permission_value;

      stream_id = screen_cast_stream_get_pipewire_node_id (stream);
      permission_value = g_strdup_printf ("%u:rwx", stream_id);
      stream_permission_values[i - 3] = permission_value;

      permission_items[i].key = PW_CORE_PROXY_PERMISSIONS_GLOBAL;
      permission_items[i].value = permission_value;
    }

  pw_core_proxy_permissions(pw_remote_get_core_proxy (remote->remote),
                            &SPA_DICT_INIT (permission_items,
                                            n_permission_items));

  g_strfreev (stream_permission_values);

  return remote;
}

void
screen_cast_stream_get_size (ScreenCastStream *stream,
                             int32_t *width,
                             int32_t *height)
{
  *width = stream->width;
  *height = stream->height;
}

void
screen_cast_stream_free (ScreenCastStream *stream)
{
  g_free (stream);
}

GList *
collect_screen_cast_stream_data (GVariantIter *streams_iter)
{
  GList *streams = NULL;
  uint32_t stream_id;
  g_autoptr(GVariant) stream_options = NULL;

  while (g_variant_iter_next (streams_iter, "(u@a{sv})",
                              &stream_id, &stream_options))
    {
      ScreenCastStream *stream;

      stream = g_new0 (ScreenCastStream, 1);
      stream->id = stream_id;
      g_variant_lookup (stream_options, "size", "(ii)",
                        &stream->width, &stream->height);

      streams = g_list_prepend (streams, stream);
    }

  return streams;
}

static gboolean
process_results (ScreenCastSession *screen_cast_session,
                 GVariant *results,
                 GError **error)
{
  g_autoptr(GVariantIter) streams_iter = NULL;

  if (!g_variant_lookup (results, "streams", "a(ua{sv})", &streams_iter))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No streams");
      return FALSE;
    }

  screen_cast_session->streams = collect_screen_cast_stream_data (streams_iter);
  return TRUE;
}

static void
start_done (GObject *source_object,
            GAsyncResult *res,
            gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  ScreenCastSession *screen_cast_session;
  guint response = 2;
  gboolean should_close_session;
  GVariant *results = NULL;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_impl_screen_cast_call_start_finish (impl,
                                               &response,
                                               &results,
                                               res,
                                               &error))
    g_warning ("A backend call failed: %s", error->message);

  should_close_session = !request->exported || response != 0;

  screen_cast_session = (ScreenCastSession *)session;

  if (request->exported)
    {
      if (response == 0)
        {
          if (!process_results (screen_cast_session, results, &error))
            {
              g_warning ("Failed to process results: %s", error->message);
              g_clear_error (&error);
              g_clear_pointer (&results, (GDestroyNotify)g_variant_unref);
              response = 2;
              should_close_session = TRUE;
            }
        }

      if (!results)
        {
          GVariantBuilder results_builder;

          g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
          results = g_variant_builder_end (&results_builder);
        }

      xdp_request_emit_response (XDP_REQUEST (request), response, results);
      request_unexport (request);
    }

  if (should_close_session)
    {
      session_close (session, TRUE);
    }
  else if (!session->closed)
    {
      g_assert (screen_cast_session->state ==
                SCREEN_CAST_SESSION_STATE_STARTING);
      g_debug ("screen cast session owned by '%s' started", session->sender);
      screen_cast_session->state = SCREEN_CAST_SESSION_STATE_STARTED;
    }
}

static gboolean
handle_start (XdpScreenCast *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  ScreenCastSession *screen_cast_session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options_builder;
  GVariant *options;

  REQUEST_AUTOLOCK (request);

  session = acquire_session (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);

  screen_cast_session = (ScreenCastSession *)session;
  switch (screen_cast_session->state)
    {
    case SCREEN_CAST_SESSION_STATE_SOURCES_SELECTED:
      break;
    case SCREEN_CAST_SESSION_STATE_INIT:
    case SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Sources not selected");
      return TRUE;
    case SCREEN_CAST_SESSION_STATE_STARTING:
    case SCREEN_CAST_SESSION_STATE_STARTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only start once");
      return TRUE;
    case SCREEN_CAST_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return TRUE;
    }

  g_object_set_data_full (G_OBJECT (request),
                          "window", g_strdup (arg_parent_window), g_free);

  impl_request =
    xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                     G_DBUS_PROXY_FLAGS_NONE,
                                     g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                     request->id,
                                     NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);
  screen_cast_session->state = SCREEN_CAST_SESSION_STATE_STARTING;

  xdp_impl_screen_cast_call_start (impl,
                                   request->id,
                                   arg_session_handle,
                                   xdp_app_info_get_id (request->app_info),
                                   arg_parent_window,
                                   options,
                                   NULL,
                                   start_done,
                                   g_object_ref (request));

  xdp_screen_cast_complete_start (object, invocation, request->id);

  return TRUE;
}

static gboolean
handle_open_pipewire_remote (XdpScreenCast *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *in_fd_list,
                             const char *arg_session_handle,
                             GVariant *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GList *streams;
  PipeWireRemote *remote;
  GUnixFDList *out_fd_list;
  int fd;
  int fd_id;
  g_autoptr(GError) error = NULL;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (is_screen_cast_session (session))
    {
      ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

      streams = screen_cast_session->streams;
    }
  else if (is_remote_desktop_session (session))
    {
      RemoteDesktopSession *remote_desktop_session =
        (RemoteDesktopSession *)session;

      streams = remote_desktop_session_get_streams (remote_desktop_session);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  if (!streams)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "No streams available");
      return TRUE;
    }

  remote = open_pipewire_screen_cast_remote (streams, &error);
  if (!remote)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "%s", error->message);
      return TRUE;
    }

  out_fd_list = g_unix_fd_list_new ();
  fd = pw_remote_steal_fd (remote->remote);
  fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);
  pipewire_remote_destroy (remote);

  if (fd_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Failed to append fd: %s",
                                             error->message);
      return TRUE;
    }

  xdp_screen_cast_complete_open_pipewire_remote (object, invocation,
                                                 out_fd_list,
                                                 g_variant_new_handle (fd_id));
  return TRUE;
}

static void
screen_cast_iface_init (XdpScreenCastIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_select_sources = handle_select_sources;
  iface->handle_start = handle_start;
  iface->handle_open_pipewire_remote = handle_open_pipewire_remote;
}

static void
sync_supported_source_types (ScreenCast *screen_cast)
{
  unsigned int available_source_types;

  available_source_types = xdp_impl_screen_cast_get_available_source_types (impl);
  xdp_screen_cast_set_available_source_types (XDP_SCREEN_CAST (screen_cast),
                                              available_source_types);
}

static void
on_supported_source_types_changed (GObject *gobject,
                                   GParamSpec *pspec,
                                   ScreenCast *screen_cast)
{
  sync_supported_source_types (screen_cast);
}

static void
screen_cast_init (ScreenCast *screen_cast)
{
  xdp_screen_cast_set_version (XDP_SCREEN_CAST (screen_cast), 1);

  g_signal_connect (impl, "notify::supported-source-types",
                    G_CALLBACK (on_supported_source_types_changed),
                    screen_cast);
  sync_supported_source_types (screen_cast);
}

static void
screen_cast_class_init (ScreenCastClass *klass)
{
  quark_request_session =
    g_quark_from_static_string ("-xdp-request-screen-cast-session");
}

GDBusInterfaceSkeleton *
screen_cast_create (GDBusConnection *connection,
                    const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_screen_cast_proxy_new_sync (connection,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              dbus_name,
                                              DESKTOP_PORTAL_OBJECT_PATH,
                                              NULL,
                                              &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create screen cast proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  screen_cast = g_object_new (screen_cast_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (screen_cast);
}

static void
screen_cast_session_close (Session *session)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

  screen_cast_session->state = SCREEN_CAST_SESSION_STATE_CLOSED;

  g_debug ("screen cast session owned by '%s' closed", session->sender);
}

static void
screen_cast_session_finalize (GObject *object)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)object;

  g_list_free_full (screen_cast_session->streams,
                    (GDestroyNotify)screen_cast_stream_free);

  G_OBJECT_CLASS (screen_cast_session_parent_class)->finalize (object);
}

static void
screen_cast_session_init (ScreenCastSession *screen_cast_session)
{
}

static void
screen_cast_session_class_init (ScreenCastSessionClass *klass)
{
  GObjectClass *object_class;
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = screen_cast_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = screen_cast_session_close;
}
