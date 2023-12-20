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
#include "restore-token.h"
#include "permissions.h"
#include "pipewire.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_ITEM(item_id, item_permissions) \
  ((struct pw_permission) { \
    .id = item_id, \
    .permissions = item_permissions \
  })
#define SCREEN_CAST_TABLE "screencast"

typedef struct _ScreenCast ScreenCast;
typedef struct _ScreenCastClass ScreenCastClass;

struct _ScreenCast
{
  XdpDbusScreenCastSkeleton parent_instance;
};

struct _ScreenCastClass
{
  XdpDbusScreenCastSkeletonClass parent_class;
};

static XdpDbusImplScreenCast *impl;
static int impl_version;
static ScreenCast *screen_cast;

static unsigned int available_cursor_modes = 0;

GType screen_cast_get_type (void);
static void screen_cast_iface_init (XdpDbusScreenCastIface *iface);

static GQuark quark_request_session;

struct _ScreenCastStream
{
  uint32_t id;
  int32_t width;
  int32_t height;
};

G_DEFINE_TYPE_WITH_CODE (ScreenCast, screen_cast,
                         XDP_DBUS_TYPE_SCREEN_CAST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SCREEN_CAST,
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
  char *restore_token;
  PersistMode persist_mode;
  GVariant *restore_data;
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

  if (session)
    g_debug ("screen cast session owned by '%s' created", session->sender);

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
  GVariantBuilder results_builder;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_dbus_impl_screen_cast_call_create_session_finish (impl,
                                                             &response,
                                                             NULL,
                                                             res,
                                                             &error))
    {
      g_dbus_error_strip_remote_error (error);
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
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
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
handle_create_session (XdpDbusScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;

  REQUEST_AUTOLOCK (request);

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = (Session *)screen_cast_session_new (arg_options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_screen_cast_call_create_session (impl,
                                                 request->id,
                                                 session->id,
                                                 xdp_app_info_get_id (request->app_info),
                                                 options,
                                                 NULL,
                                                 create_session_done,
                                                 g_object_ref (request));

  xdp_dbus_screen_cast_complete_create_session (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
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

  if (!xdp_dbus_impl_screen_cast_call_select_sources_finish (impl,
                                                             &response,
                                                             &results,
                                                             res,
                                                             &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  should_close_session = !request->exported || response != 0;

  if (request->exported)
    {
      if (!results)
        {
          GVariantBuilder results_builder;

          g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
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

static gboolean
validate_source_types (const char *key,
                       GVariant *value,
                       GVariant *options,
                       GError **error)
{
  guint32 types = g_variant_get_uint32 (value);

  if ((types & ~(1 | 2 | 4)) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unsupported device type: %x", types & ~(1 | 2 | 4));
      return FALSE;
    } 

  return TRUE;
}

static gboolean
validate_cursor_mode (const char *key,
                      GVariant *value,
                      GVariant *options,
                      GError **error)
{
  uint32_t mode = g_variant_get_uint32 (value);

  if (__builtin_popcount (mode) != 1)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid cursor mode %x", mode);
      return FALSE;
    }

  if (!(available_cursor_modes & mode))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unavailable cursor mode %x", mode);
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_restore_token (const char *key,
                        GVariant *value,
                        GVariant *options,
                        GError **error)
{
  const char *restore_token = g_variant_get_string (value, NULL);

  if (!g_uuid_string_is_valid (restore_token))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Restore token is not a valid UUID string");
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_persist_mode (const char *key,
                       GVariant *value,
                       GVariant *options,
                       GError **error)
{
  uint32_t mode = g_variant_get_uint32 (value);

  if (mode > PERSIST_MODE_PERSISTENT)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid persist mode %x", mode);
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey screen_cast_select_sources_options[] = {
  { "types", G_VARIANT_TYPE_UINT32, validate_source_types },
  { "multiple", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "cursor_mode", G_VARIANT_TYPE_UINT32, validate_cursor_mode },
  { "restore_token", G_VARIANT_TYPE_STRING, validate_restore_token },
  { "persist_mode", G_VARIANT_TYPE_UINT32, validate_persist_mode },
};

static gboolean
replace_screen_cast_restore_token_with_data (Session *session,
                                             GVariant **in_out_options,
                                             GError **error)
{
  g_autoptr(GVariant) options = NULL;
  PersistMode persist_mode;

  options = *in_out_options;

  if (!g_variant_lookup (options, "persist_mode", "u", &persist_mode))
    persist_mode = PERSIST_MODE_NONE;

  if (is_remote_desktop_session (session))
    {
      if (persist_mode != PERSIST_MODE_NONE ||
          xdp_variant_contains_key (options, "restore_token"))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Remote desktop sessions cannot persist");
          return FALSE;
        }
    }

  if (is_screen_cast_session (session))
    {
      ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

      screen_cast_session->persist_mode = persist_mode;
      xdp_session_persistence_replace_restore_token_with_data (session,
                                                               SCREEN_CAST_TABLE,
                                                               in_out_options,
                                                               &screen_cast_session->restore_token);
    }
  else
    {
      *in_out_options = g_steal_pointer (&options);
    }

  return TRUE;
}

static gboolean
handle_select_sources (XdpDbusScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        case SCREEN_CAST_SESSION_STATE_STARTING:
        case SCREEN_CAST_SESSION_STATE_STARTED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Can only select sources before starting");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        case SCREEN_CAST_SESSION_STATE_CLOSED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Invalid session");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
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
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           screen_cast_select_sources_options,
                           G_N_ELEMENTS (screen_cast_select_sources_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  /* If 'restore_token' is passed, lookup the corresponding data in the
   * permission store and / or the GHashTable with transient permissions.
   * Portal implementations do not have access to the restore token.
   */
  if (!replace_screen_cast_restore_token_with_data (session, &options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);
  if (is_screen_cast_session (session))
    {
      ((ScreenCastSession *)session)->state =
        SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES;
    }

  xdp_dbus_impl_screen_cast_call_select_sources (impl,
                                                 request->id,
                                                 arg_session_handle,
                                                 xdp_app_info_get_id (request->app_info),
                                                 options,
                                                 NULL,
                                                 select_sources_done,
                                                 g_object_ref (request));

  xdp_dbus_screen_cast_complete_select_sources (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

uint32_t
screen_cast_stream_get_pipewire_node_id (ScreenCastStream *stream)
{
  return stream->id;
}

static void
append_stream_permissions (PipeWireRemote *remote,
                           GArray *permission_items,
                           GList *streams)
{
  GList *l;

  for (l = streams; l; l = l->next)
    {
      ScreenCastStream *stream = l->data;
      uint32_t stream_id;

      stream_id = screen_cast_stream_get_pipewire_node_id (stream);
      g_array_append_val (permission_items,
                          PERMISSION_ITEM (stream_id, PW_PERM_RWX));
    }
}

static PipeWireRemote *
open_pipewire_screen_cast_remote (const char *app_id,
                                  GList *streams,
                                  GError **error)
{
  struct pw_properties *pipewire_properties;
  PipeWireRemote *remote;
  g_autoptr(GArray) permission_items = NULL;

  pipewire_properties = pw_properties_new ("pipewire.access.portal.app_id", app_id,
                                           "pipewire.access.portal.media_roles", "",
                                           NULL);
  remote = pipewire_remote_new_sync (pipewire_properties,
                                     NULL, NULL, NULL, NULL,
                                     error);
  if (!remote)
    return FALSE;

  permission_items = g_array_new (FALSE, TRUE, sizeof (struct pw_permission));

  /*
   * PipeWire:Interface:Core
   * Needs rwx to be able create the sink node using the create-object method
   */
  g_array_append_val (permission_items,
                      PERMISSION_ITEM (PW_ID_CORE, PW_PERM_RWX));

  /*
   * PipeWire:Interface:NodeFactory
   * Needs r-- so it can be passed to create-object when creating the sink node.
   */
  g_array_append_val (permission_items,
                      PERMISSION_ITEM (remote->node_factory_id, PW_PERM_R));

  append_stream_permissions (remote, permission_items, streams);

  /*
   * Hide all existing and future nodes (except the ones we explicitly list above).
   */
  g_array_append_val (permission_items,
                      PERMISSION_ITEM (PW_ID_ANY, 0));

  pw_client_update_permissions (pw_core_get_client(remote->core),
                                permission_items->len,
                                (const struct pw_permission *)permission_items->data);

  pipewire_remote_roundtrip (remote);

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

static void
replace_restore_screen_cast_data_with_token (ScreenCastSession *screen_cast_session,
                                             GVariant **in_out_results)
{
  xdp_session_persistence_replace_restore_data_with_token ((Session *) screen_cast_session,
                                                           SCREEN_CAST_TABLE,
                                                           in_out_results,
                                                           &screen_cast_session->persist_mode,
                                                           &screen_cast_session->restore_token,
                                                           &screen_cast_session->restore_data);
}

static gboolean
process_results (ScreenCastSession *screen_cast_session,
                 GVariant **in_out_results,
                 GError **error)
{
  g_autoptr(GVariantIter) streams_iter = NULL;
  GVariant *results = *in_out_results;

  if (!g_variant_lookup (results, "streams", "a(ua{sv})", &streams_iter))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No streams");
      return FALSE;
    }

  screen_cast_session->streams = collect_screen_cast_stream_data (streams_iter);
  replace_restore_screen_cast_data_with_token (screen_cast_session,
                                               in_out_results);
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

  if (!xdp_dbus_impl_screen_cast_call_start_finish (impl,
                                                    &response,
                                                    &results,
                                                    res,
                                                    &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  should_close_session = !request->exported || response != 0;

  screen_cast_session = (ScreenCastSession *)session;

  if (request->exported)
    {
      if (response == 0)
        {
          if (!process_results (screen_cast_session, &results, &error))
            {
              g_warning ("Failed to process results: %s", error->message);
              g_clear_error (&error);
              g_clear_pointer (&results, g_variant_unref);
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

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
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
handle_start (XdpDbusScreenCast *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  ScreenCastSession *screen_cast_session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case SCREEN_CAST_SESSION_STATE_STARTING:
    case SCREEN_CAST_SESSION_STATE_STARTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only start once");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case SCREEN_CAST_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (request),
                          "window", g_strdup (arg_parent_window), g_free);

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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

  xdp_dbus_impl_screen_cast_call_start (impl,
                                        request->id,
                                        arg_session_handle,
                                        xdp_app_info_get_id (request->app_info),
                                        arg_parent_window,
                                        options,
                                        NULL,
                                        start_done,
                                        g_object_ref (request));

  xdp_dbus_screen_cast_complete_start (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_open_pipewire_remote (XdpDbusScreenCast *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *in_fd_list,
                             const char *arg_session_handle,
                             GVariant *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GList *streams;
  PipeWireRemote *remote;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!streams)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "No streams available");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  remote = open_pipewire_screen_cast_remote (session->app_id, streams, &error);
  if (!remote)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "%s", error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  out_fd_list = g_unix_fd_list_new ();
  fd = pw_core_steal_fd (remote->core);
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_screen_cast_complete_open_pipewire_remote (object, invocation,
                                                      out_fd_list,
                                                      g_variant_new_handle (fd_id));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
screen_cast_iface_init (XdpDbusScreenCastIface *iface)
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

  available_source_types = xdp_dbus_impl_screen_cast_get_available_source_types (impl);
  xdp_dbus_screen_cast_set_available_source_types (XDP_DBUS_SCREEN_CAST (screen_cast),
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
sync_supported_cursor_modes (ScreenCast *screen_cast)
{

  available_cursor_modes = xdp_dbus_impl_screen_cast_get_available_cursor_modes (impl);
  xdp_dbus_screen_cast_set_available_cursor_modes (XDP_DBUS_SCREEN_CAST (screen_cast),
                                                   available_cursor_modes);
}

static void
on_supported_cursor_modes_changed (GObject *gobject,
                                   GParamSpec *pspec,
                                   ScreenCast *screen_cast)
{
  sync_supported_cursor_modes (screen_cast);
}

static void
screen_cast_init (ScreenCast *screen_cast)
{
  xdp_dbus_screen_cast_set_version (XDP_DBUS_SCREEN_CAST (screen_cast), 5);

  g_signal_connect (impl, "notify::supported-source-types",
                    G_CALLBACK (on_supported_source_types_changed),
                    screen_cast);
  if (impl_version >= 2)
    {
      g_signal_connect (impl, "notify::supported-cursor-modes",
                        G_CALLBACK (on_supported_cursor_modes_changed),
                        screen_cast);
    }
  sync_supported_source_types (screen_cast);
  sync_supported_cursor_modes (screen_cast);
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

  impl = xdp_dbus_impl_screen_cast_proxy_new_sync (connection,
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

  impl_version = xdp_dbus_impl_screen_cast_get_version (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  screen_cast = g_object_new (screen_cast_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (screen_cast);
}

static void
screen_cast_session_close (Session *session)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

  screen_cast_session->state = SCREEN_CAST_SESSION_STATE_CLOSED;

  xdp_session_persistence_generate_and_save_restore_token (session,
                                                           SCREEN_CAST_TABLE,
                                                           screen_cast_session->persist_mode,
                                                           &screen_cast_session->restore_token,
                                                           &screen_cast_session->restore_data);

  g_debug ("screen cast session owned by '%s' closed", session->sender);
}

static void
screen_cast_session_finalize (GObject *object)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)object;

  g_clear_pointer (&screen_cast_session->restore_token, g_free);
  g_clear_pointer (&screen_cast_session->restore_data, g_variant_unref);

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
