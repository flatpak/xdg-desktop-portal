/*
 * Copyright © 2017-2018 Red Hat, Inc
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

#include "config.h"

#include <glib/gi18n.h>
#include <gio/gdesktopappinfo.h>

#include <stdint.h>
#include <pipewire/pipewire.h>
#include <gio/gunixfdlist.h>

#include "xdp-session.h"
#include "screen-cast.h"
#include "remote-desktop.h"
#include "xdp-request.h"
#include "xdp-permissions.h"
#include "pipewire.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-session-persistence.h"
#include "xdp-utils.h"

#define PERMISSION_ITEM(item_id, item_permissions) \
  ((struct pw_permission) { \
    .id = item_id, \
    .permissions = item_permissions \
  })
#define SCREEN_CAST_TABLE "screencast"

#define PROVIDER_ID "provider"

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
static XdpDbusImplAccess *access_impl = NULL;
static int impl_version;
static ScreenCast *screen_cast;

static unsigned int available_source_types = 0;
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
  XdpSession parent;

  ScreenCastSessionState state;

  GList *streams;
  char *restore_token;
  XdpSessionPersistenceMode persist_mode;
  GVariant *restore_data;
} ScreenCastSession;

typedef struct _ScreenCastSessionClass
{
  XdpSessionClass parent_class;
} ScreenCastSessionClass;

GType screen_cast_session_get_type (void);

G_DEFINE_TYPE (ScreenCastSession, screen_cast_session, xdp_session_get_type ())

G_GNUC_UNUSED static inline ScreenCastSession *
SCREEN_CAST_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, screen_cast_session_get_type (), ScreenCastSession);
}

G_GNUC_UNUSED static inline gboolean
IS_SCREEN_CAST_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, screen_cast_session_get_type ());
}

typedef enum _ScreenCastProviderSessionState
{
  SCREEN_CAST_PROVIDER_SESSION_STATE_INIT,
  SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTING,
  SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTED,
  SCREEN_CAST_PROVIDER_SESSION_STATE_CLOSED
} ScreenCastProviderSessionState;

typedef struct _ScreenCastProviderSession
{
  XdpSession parent;

  ScreenCastProviderSessionState state;
} ScreenCastProviderSession;

typedef struct _ScreenCastProviderSessionClass
{
  XdpSessionClass parent_class;
} ScreenCastProviderSessionClass;

GType screen_cast_provider_session_get_type (void);

G_DEFINE_TYPE (ScreenCastProviderSession, screen_cast_provider_session, xdp_session_get_type ())

G_GNUC_UNUSED static inline ScreenCastProviderSession *
SCREEN_CAST_PROVIDER_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, screen_cast_provider_session_get_type (),
                                     ScreenCastProviderSession);
}

G_GNUC_UNUSED static inline gboolean
IS_SCREEN_CAST_PROVIDER_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, screen_cast_provider_session_get_type ());
}

static ScreenCastSession *
screen_cast_session_new (GVariant *options,
                         XdpRequest *request,
                         GError **error)
{
  XdpSession *session;
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

static ScreenCastProviderSession *
screen_cast_provider_session_new (GVariant   *options,
                                  XdpRequest *request,
                                  GError    **error)
{
  XdpSession *session;
  GDBusInterfaceSkeleton *interface_skeleton =
    G_DBUS_INTERFACE_SKELETON (request);
  const char *session_token;
  GDBusConnection *connection =
    g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (screen_cast_provider_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("screen cast provider session owned by '%s' created", session->sender);

  return (ScreenCastProviderSession*)session;
}

static void
create_session_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  g_autoptr(XdpRequest) request = data;
  XdpSession *session;
  guint response = 2;
  gboolean should_close_session;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

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
      if (!xdp_session_export (session, &error))
        {
          g_warning ("Failed to export session: %s", error->message);
          response = 2;
          should_close_session = TRUE;
          goto out;
        }

      should_close_session = FALSE;
      xdp_session_register (session);
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
      xdp_request_unexport (request);
    }

  if (should_close_session)
    xdp_session_close (session, FALSE);
}

static XdpOptionKey create_session_options[] = {
  { "provider", G_VARIANT_TYPE_BOOLEAN, NULL },
};

static gboolean
handle_create_session (XdpDbusScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  gboolean provider;

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

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  if (!g_variant_lookup (arg_options, "provider", "b", &provider))
    provider = FALSE;

  if (provider &&
      ((available_source_types & 8) == 0 || access_impl == NULL || impl_version < 6))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Creating provider session is not available");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (provider)
    session = XDP_SESSION (screen_cast_provider_session_new (arg_options, request, &error));
  else
    session = XDP_SESSION (screen_cast_session_new (arg_options, request, &error));
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_filter_options (arg_options, &options_builder,
                      create_session_options, G_N_ELEMENTS (create_session_options),
                      NULL);
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

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
  g_autoptr(XdpRequest) request = data;
  XdpSession *session;
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
          g_auto(GVariantBuilder) results_builder =
            G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      xdp_request_unexport (request);
    }

  if (should_close_session)
    {
      xdp_session_close (session, TRUE);
    }
  else if (!session->closed)
    {
      if (IS_SCREEN_CAST_SESSION (session))
        {
          ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (session);

          g_assert_cmpint (screen_cast_session->state,
                           ==,
                           SCREEN_CAST_SESSION_STATE_SELECTING_SOURCES);
          screen_cast_session->state = SCREEN_CAST_SESSION_STATE_SOURCES_SELECTED;
        }
      else if (IS_REMOTE_DESKTOP_SESSION (session))
        {
          RemoteDesktopSession *remote_desktop_session =
            REMOTE_DESKTOP_SESSION (session);

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

  if ((types & ~(1 | 2 | 4 | 8)) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unsupported device type: %x", types & ~(1 | 2 | 4 | 8));
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

  if (mode > XDP_SESSION_PERSISTENCE_MODE_PERSISTENT)
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
replace_screen_cast_restore_token_with_data (XdpSession *session,
                                             GVariant **in_out_options,
                                             GError **error)
{
  g_autoptr(GVariant) options = NULL;
  XdpSessionPersistenceMode persist_mode;

  options = *in_out_options;

  if (!g_variant_lookup (options, "persist_mode", "u", &persist_mode))
    persist_mode = XDP_SESSION_PERSISTENCE_MODE_NONE;

  if (IS_REMOTE_DESKTOP_SESSION (session))
    {
      if (persist_mode != XDP_SESSION_PERSISTENCE_MODE_NONE ||
          xdp_variant_contains_key (options, "restore_token"))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Remote desktop sessions cannot persist");
          return FALSE;
        }
    }

  if (IS_SCREEN_CAST_SESSION (session))
    {
      ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (session);

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
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  session = xdp_session_from_request (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (IS_SCREEN_CAST_SESSION (session))
    {
      ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (session);

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
  else if (IS_REMOTE_DESKTOP_SESSION (session))
    {
      RemoteDesktopSession *remote_desktop_session =
        REMOTE_DESKTOP_SESSION (session);

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

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  if (!xdp_filter_options (arg_options, &options_builder,
                           screen_cast_select_sources_options,
                           G_N_ELEMENTS (screen_cast_select_sources_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

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
  if (IS_SCREEN_CAST_SESSION (session))
    {
      SCREEN_CAST_SESSION (session)->state =
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

  if (streams)
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
  xdp_session_persistence_replace_restore_data_with_token (XDP_SESSION (screen_cast_session),
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
  g_autoptr(XdpRequest) request = data;
  XdpSession *session;
  ScreenCastSession *screen_cast_session;
  guint response = 2;
  gboolean should_close_session;
  g_autoptr(GVariant) results = NULL;
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
      g_clear_error (&error);
    }

  should_close_session = !request->exported || response != 0;

  screen_cast_session = SCREEN_CAST_SESSION (session);

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
          g_auto(GVariantBuilder) results_builder =
            G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      xdp_request_unexport (request);
    }

  if (should_close_session)
    {
      xdp_session_close (session, TRUE);
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
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  ScreenCastSession *screen_cast_session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  session = xdp_session_from_request (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  screen_cast_session = SCREEN_CAST_SESSION (session);
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

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

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
  XdpCall *call = xdp_call_from_invocation (invocation);
  XdpSession *session;
  gboolean is_provider_session;
  GList *streams = NULL;
  PipeWireRemote *remote;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int fd;
  int fd_id;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  is_provider_session = IS_SCREEN_CAST_PROVIDER_SESSION (session);
  if (IS_SCREEN_CAST_SESSION (session))
    {
      ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (session);

      streams = screen_cast_session->streams;
    }
  else if (IS_REMOTE_DESKTOP_SESSION (session))
    {
      RemoteDesktopSession *remote_desktop_session =
        REMOTE_DESKTOP_SESSION (session);

      streams = remote_desktop_session_get_streams (remote_desktop_session);
    }
  else if (!is_provider_session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!streams && !is_provider_session)
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
send_response (XdpRequest *request,
               XdpSession *session,
               guint response,
               GVariant *results)
{
  if (request->exported)
    {
      g_debug ("sending response: %d", response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      xdp_request_unexport (request);
    }

  if (response != 0)
    {
      g_debug ("closing session");
      xdp_session_close (session, FALSE);
    }
}

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  XdpRequest *request = task_data;
  XdpSession *session;
  ScreenCastProviderSession *screen_cast_provider_session;
  guint response;
  GVariant *results;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  response = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (request), "response"));
  results = (GVariant *)g_object_get_data (G_OBJECT (request), "results");

  if (response == 0)
    {
      screen_cast_provider_session = SCREEN_CAST_PROVIDER_SESSION (session);
      g_assert (screen_cast_provider_session->state ==
                SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTING);
      screen_cast_provider_session->state =
        SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTED;
    }

  if (!results)
    {
      GVariantBuilder results_builder;

      g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
      results = g_variant_builder_end (&results_builder);
    }

  send_response (request, session, response, results);
}

static void
connect_provisioning_done (GObject *source,
                           GAsyncResult *res,
                           gpointer data)
{
  g_autoptr (XdpRequest) request = data;
  guint response = 2;
  g_autoptr (GVariant) results = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GTask) task = NULL;

  if (!xdp_dbus_impl_screen_cast_call_connect_provisioning_finish (impl,
                                                                   &response,
                                                                   &results,
                                                                   NULL,
                                                                   res,
                                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GUINT_TO_POINTER (response));
  if (results)
    g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static void
handle_connect_provisioning_in_thread_func (GTask *task,
                                            gpointer source_object,
                                            gpointer task_data,
                                            GCancellable *cancellable)
{
  XdpRequest *request = XDP_REQUEST (task_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (XdpDbusImplRequest) impl_request = NULL;
  XdpSession *session;
  g_auto (GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  XdpPermission permission;
  GUnixFDList *fd_list;
  const gchar *parent_window;
  GVariant *fd;
  const gchar *app_id;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  app_id = xdp_app_info_get_id (request->app_info);
  fd_list = ((GUnixFDList *)g_object_get_data (G_OBJECT (request), "fd-list"));
  parent_window = ((const gchar *)g_object_get_data (G_OBJECT (request), "parent-window"));
  fd = ((GVariant *)g_object_get_data (G_OBJECT (request), "fd"));

  permission = xdp_get_permission_sync (app_id, SCREEN_CAST_TABLE, PROVIDER_ID);

  if (permission != XDP_PERMISSION_YES)
    {
      g_autoptr (GVariant) access_results = NULL;
      g_auto(GVariantBuilder) access_opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autofree gchar *subtitle = NULL;
      g_autofree gchar *title = NULL;
      const gchar *body;
      guint access_response = 2;

      if (permission == XDP_PERMISSION_NO)
        {
          send_response (request, session, 2, g_variant_builder_end (&opt_builder));
          return;
        }

      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Allow")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("screen-shared-symbolic"));

      if (g_strcmp0 (app_id, "") != 0)
        {
          g_autoptr(GDesktopAppInfo) info = NULL;
          g_autofree gchar *id = NULL;
          const gchar *name = NULL;

          id = g_strconcat (app_id, ".desktop", NULL);
          info = g_desktop_app_info_new (id);

          if (info)
            name = g_app_info_get_display_name (G_APP_INFO (info));
          else
            name = app_id;

          title = g_strdup_printf (_("Allow %s to Provide Screen Cast Sources?"), name);
          subtitle = g_strdup_printf (_("%s wants to enable other applications to share its content."), name);
        }
      else
        {
          /* Note: this will set the screencast provider permission for all unsandboxed
           * apps for which an app ID can't be determined.
           */
          g_assert (xdp_app_info_is_host (request->app_info));
          title = g_strdup (_("Allow Applications to Provide Screen Cast Sources?"));
          subtitle = g_strdup (_("An application wants to enable other applications to share its content."));
        }
      body = _("This permission can be changed at any time from the privacy settings.");

      if (!xdp_dbus_impl_access_call_access_dialog_sync (access_impl,
                                                         request->id,
                                                         app_id,
                                                         parent_window,
                                                         title,
                                                         subtitle,
                                                         body,
                                                         g_variant_builder_end (&access_opt_builder),
                                                         &access_response,
                                                         &access_results,
                                                         NULL,
                                                         &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);
          send_response (request, session, 2, g_variant_builder_end (&opt_builder));
          return;
        }

      if (permission == XDP_PERMISSION_UNSET)
        xdp_set_permission_sync (app_id, SCREEN_CAST_TABLE, PROVIDER_ID, access_response == 0 ? XDP_PERMISSION_YES : XDP_PERMISSION_NO);

      if (access_response != 0)
        {
          send_response (request, session, 2, g_variant_builder_end (&opt_builder));
          return;
        }
    }

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);

  if (!impl_request)
    {
      g_warning ("Failed to create screen cast implementation proxy: %s", error->message);
      send_response (request, session, 2, g_variant_builder_end (&opt_builder));
      return;
    }

  xdp_request_set_impl_request (request, impl_request);
  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  g_debug ("Calling ConnectProvisioning with app_id=%s", app_id);
  xdp_dbus_impl_screen_cast_call_connect_provisioning (impl,
                                                       request->id,
                                                       session->id,
                                                       app_id,
                                                       fd,
                                                       g_variant_builder_end (&opt_builder),
                                                       fd_list,
                                                       NULL,
                                                       connect_provisioning_done,
                                                       g_object_ref (request));
}

static gboolean
handle_connect_provisioning (XdpDbusScreenCast *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *fd_list,
                             const char *arg_session_handle,
                             const gchar *arg_parent_window,
                             GVariant *arg_fd,
                             GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  ScreenCastProviderSession *screen_cast_provider_session;
  g_autoptr (GTask) task = NULL;

  g_debug ("Handle ConnectProvisioning");

  REQUEST_AUTOLOCK (request);

  session = xdp_session_from_request (arg_session_handle, request);
  if (!session || !IS_SCREEN_CAST_PROVIDER_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  screen_cast_provider_session = SCREEN_CAST_PROVIDER_SESSION (session);
  switch (screen_cast_provider_session->state)
    {
    case SCREEN_CAST_PROVIDER_SESSION_STATE_INIT:
      break;
    case SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTING:
    case SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only add provider once");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case SCREEN_CAST_PROVIDER_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_object_set_data_full (G_OBJECT (request), "fd-list", g_object_ref (fd_list), g_object_unref);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "fd", g_variant_ref (arg_fd), (GDestroyNotify)g_variant_unref);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  screen_cast_provider_session->state =
    SCREEN_CAST_PROVIDER_SESSION_STATE_CONNECTING;

  xdp_dbus_screen_cast_complete_connect_provisioning (object, invocation, NULL, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_connect_provisioning_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
screen_cast_iface_init (XdpDbusScreenCastIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_select_sources = handle_select_sources;
  iface->handle_start = handle_start;
  iface->handle_open_pipewire_remote = handle_open_pipewire_remote;
  iface->handle_connect_provisioning = handle_connect_provisioning;
}

static void
sync_supported_source_types (ScreenCast *screen_cast)
{
  available_source_types = xdp_dbus_impl_screen_cast_get_available_source_types (impl);

  /* External type is never available if no Access portal implementation is
   * found */
  if ((available_source_types & 8) != 0 && access_impl == NULL)
    available_source_types ^= 8;

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
  xdp_dbus_screen_cast_set_version (XDP_DBUS_SCREEN_CAST (screen_cast), 6);

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
                    const char *dbus_name_access,
                    const char *dbus_name_screen_cast)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_screen_cast_proxy_new_sync (connection,
                                                   G_DBUS_PROXY_FLAGS_NONE,
                                                   dbus_name_screen_cast,
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

  if (dbus_name_access != NULL || impl_version >= 6)
    access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       dbus_name_access,
                                                       DESKTOP_PORTAL_OBJECT_PATH,
                                                       NULL,
                                                       &error);

  screen_cast = g_object_new (screen_cast_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (screen_cast);
}

static void
screen_cast_session_close (XdpSession *session)
{
  ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (session);

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
  ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (object);

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
  XdpSessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = screen_cast_session_finalize;

  session_class = (XdpSessionClass *)klass;
  session_class->close = screen_cast_session_close;
}

static void
screen_cast_provider_session_close (XdpSession *session)
{
  ScreenCastProviderSession *screen_cast_provider_session =
    SCREEN_CAST_PROVIDER_SESSION (session);

  screen_cast_provider_session->state = SCREEN_CAST_PROVIDER_SESSION_STATE_CLOSED;

  g_debug ("screen cast provider session owned by '%s' closed", session->sender);
}

static void
screen_cast_provider_session_init (ScreenCastProviderSession *screen_cast_provider_session)
{
};

static void
screen_cast_provider_session_class_init (ScreenCastProviderSessionClass *klass)
{
  XdpSessionClass *session_class;

  session_class = (XdpSessionClass *)klass;
  session_class->close = screen_cast_provider_session_close;
}
