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

#include "remote-desktop.h"
#include "screen-cast.h"
#include "request.h"
#include "pipewire.h"
#include "call.h"
#include "session.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#include <stdint.h>

typedef struct _RemoteDesktop RemoteDesktop;
typedef struct _RemoteDesktopClass RemoteDesktopClass;

struct _RemoteDesktop
{
  XdpRemoteDesktopSkeleton parent_instance;
};

struct _RemoteDesktopClass
{
  XdpRemoteDesktopSkeletonClass parent_class;
};

static XdpImplRemoteDesktop *impl;
static RemoteDesktop *remote_desktop;

GType remote_desktop_get_type (void) G_GNUC_CONST;
static void remote_desktop_iface_init (XdpRemoteDesktopIface *iface);

static GQuark quark_request_session;

G_DEFINE_TYPE_WITH_CODE (RemoteDesktop, remote_desktop, XDP_TYPE_REMOTE_DESKTOP_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_REMOTE_DESKTOP,
                                                remote_desktop_iface_init))

typedef enum _RemoteDesktopSessionState
{
  REMOTE_DESKTOP_SESSION_STATE_INIT,
  REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES,
  REMOTE_DESKTOP_SESSION_STATE_DEVICES_SELECTED,
  REMOTE_DESKTOP_SESSION_STATE_SELECTING_SOURCES,
  REMOTE_DESKTOP_SESSION_STATE_SOURCES_SELECTED,
  REMOTE_DESKTOP_SESSION_STATE_STARTING,
  REMOTE_DESKTOP_SESSION_STATE_STARTED,
  REMOTE_DESKTOP_SESSION_STATE_CLOSED
} RemoteDesktopSessionState;

typedef enum _DeviceType
{
  DEVICE_TYPE_NONE = 0,
  DEVICE_TYPE_KEYBOARD = 1 << 0,
  DEVICE_TYPE_POINTER = 1 << 1,
  DEVICE_TYPE_TOUCHSCREEN = 1 << 2,
} DeviceType;

typedef struct _RemoteDesktopSession
{
  Session parent;

  RemoteDesktopSessionState state;

  DeviceType shared_devices;

  GList *streams;
} RemoteDesktopSession;

typedef struct _RemoteDesktopSessionClass
{
  SessionClass parent_class;
} RemoteDesktopSessionClass;

GType remote_desktop_session_get_type (void);

G_DEFINE_TYPE (RemoteDesktopSession, remote_desktop_session, session_get_type ())

gboolean
is_remote_desktop_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session,
                                     remote_desktop_session_get_type ());
}

gboolean
remote_desktop_session_can_select_sources (RemoteDesktopSession *session)
{

  switch (session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES:
    case REMOTE_DESKTOP_SESSION_STATE_DEVICES_SELECTED:
      return TRUE;
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_SOURCES:
    case REMOTE_DESKTOP_SESSION_STATE_SOURCES_SELECTED:
    case REMOTE_DESKTOP_SESSION_STATE_STARTING:
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      return FALSE;
    }

  g_assert_not_reached ();
}

GList *
remote_desktop_session_get_streams (RemoteDesktopSession *session)
{
  return session->streams;
}

void
remote_desktop_session_selecting_sources (RemoteDesktopSession *session)
{
  session->state = REMOTE_DESKTOP_SESSION_STATE_SELECTING_SOURCES;
}

void
remote_desktop_session_sources_selected (RemoteDesktopSession *session)
{
  session->state = REMOTE_DESKTOP_SESSION_STATE_SOURCES_SELECTED;
}

static RemoteDesktopSession *
remote_desktop_session_new (GVariant *options,
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
  session = g_initable_new (remote_desktop_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("remote desktop session owned by '%s' created", session->sender);

  return (RemoteDesktopSession *)session;
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

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_impl_remote_desktop_call_create_session_finish (impl,
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
handle_create_session (XdpRemoteDesktop *object,
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = (Session *)remote_desktop_session_new (arg_options, request, &error);
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

  xdp_impl_remote_desktop_call_create_session (impl,
                                               request->id,
                                               session->id,
                                               xdp_app_info_get_id (request->app_info),
                                               options,
                                               NULL,
                                               create_session_done,
                                               g_object_ref (request));

  xdp_remote_desktop_complete_create_session (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
select_devices_done (GObject *source_object,
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

  if (!xdp_impl_remote_desktop_call_select_devices_finish (impl,
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

      xdp_request_emit_response (XDP_REQUEST (request), response, results);
      request_unexport (request);
    }

  if (should_close_session)
    {
      session_close (session, TRUE);
    }
  else if (!session->closed)
    {
      RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)session;

      g_assert_cmpint (remote_desktop_session->state,
                       ==,
                       REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES);
      remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_DEVICES_SELECTED;
    }
}

static gboolean
validate_device_types (const char *key,
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

static XdpOptionKey remote_desktop_select_devices_options[] = {
  { "types", G_VARIANT_TYPE_UINT32, validate_device_types },
};

static gboolean
handle_select_devices (XdpRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  RemoteDesktopSession *remote_desktop_session;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  remote_desktop_session = (RemoteDesktopSession *)session;
  switch (remote_desktop_session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_SOURCES:
    case REMOTE_DESKTOP_SESSION_STATE_SOURCES_SELECTED:
      break;
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES:
    case REMOTE_DESKTOP_SESSION_STATE_DEVICES_SELECTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Sources already selected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case REMOTE_DESKTOP_SESSION_STATE_STARTING:
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only select devices before starting");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_select_devices_options,
                           G_N_ELEMENTS (remote_desktop_select_devices_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);
  remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES;

  xdp_impl_remote_desktop_call_select_devices (impl,
                                               request->id,
                                               arg_session_handle,
                                               xdp_app_info_get_id (request->app_info),
                                               g_variant_builder_end (&options_builder),
                                               NULL,
                                               select_devices_done,
                                               g_object_ref (request));

  xdp_remote_desktop_complete_select_devices (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
process_results (RemoteDesktopSession *remote_desktop_session,
                 GVariant *results,
                 GError **error)
{
  g_autoptr(GVariantIter) streams_iter = NULL;
  uint32_t devices = 0;

  if (g_variant_lookup (results, "streams", "a(ua{sv})", &streams_iter))
    {
      remote_desktop_session->streams =
        collect_screen_cast_stream_data (streams_iter);
    }

  if (g_variant_lookup (results, "devices", "u", &devices))
    remote_desktop_session->shared_devices = devices;

  return TRUE;
}

static void
start_done (GObject *source_object,
            GAsyncResult *res,
            gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  RemoteDesktopSession *remote_desktop_session;
  guint response = 2;
  gboolean should_close_session;
  GVariant *results = NULL;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  remote_desktop_session = (RemoteDesktopSession *)session;
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_impl_remote_desktop_call_start_finish (impl,
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
      if (response == 0)
        {
          if (!process_results (remote_desktop_session, results, &error))
            {
              g_warning ("Could not start remote desktop session: %s",
                         error->message);
              g_clear_error (&error);
              g_clear_pointer (&results, g_variant_unref);
              response = 2;
              should_close_session = TRUE;
            }
        }
      else
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
      g_assert (remote_desktop_session->state ==
                REMOTE_DESKTOP_SESSION_STATE_STARTING);
      g_debug ("remote desktop session owned by '%s' started", session->sender);
      remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_STARTED;
    }
}

static gboolean
handle_start (XdpRemoteDesktop *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  RemoteDesktopSession *remote_desktop_session;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  remote_desktop_session = (RemoteDesktopSession *)session;
  switch (remote_desktop_session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
    case REMOTE_DESKTOP_SESSION_STATE_DEVICES_SELECTED:
    case REMOTE_DESKTOP_SESSION_STATE_SOURCES_SELECTED:
      break;
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_SOURCES:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Sources not selected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Devices not selected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case REMOTE_DESKTOP_SESSION_STATE_STARTING:
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only start once");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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
  remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_STARTING;

  xdp_impl_remote_desktop_call_start (impl,
                                      request->id,
                                      arg_session_handle,
                                      xdp_app_info_get_id (request->app_info),
                                      arg_parent_window,
                                      options,
                                      NULL,
                                      start_done,
                                      g_object_ref (request));

  xdp_remote_desktop_complete_start (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
check_notify (Session *session,
              DeviceType device_type)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)session;

  switch (remote_desktop_session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
      break;
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
    case REMOTE_DESKTOP_SESSION_STATE_DEVICES_SELECTED:
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_DEVICES:
    case REMOTE_DESKTOP_SESSION_STATE_SOURCES_SELECTED:
    case REMOTE_DESKTOP_SESSION_STATE_SELECTING_SOURCES:
    case REMOTE_DESKTOP_SESSION_STATE_STARTING:
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      return FALSE;
    }

  if ((remote_desktop_session->shared_devices & device_type) == 0)
    return FALSE;

  return TRUE;
}

static gboolean
check_position (Session *session,
                uint32_t stream,
                double x,
                double y)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)session;
  GList *l;

  for (l = remote_desktop_session->streams; l; l = l->next)
    {
      ScreenCastStream *stream = l->data;
      int32_t width, height;

      screen_cast_stream_get_size (stream, &width, &height);

      if (x >= 0.0 && x < width &&
          y >= 0.0 && y < height)
        return TRUE;
    }

  return FALSE;
}

static XdpOptionKey remote_desktop_notify_options[] = {
};

static gboolean
handle_notify_pointer_motion (XdpRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              double dx,
                              double dy)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_POINTER))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: pointer");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_pointer_motion (impl,
                                                      session->id,
                                                      options,
                                                      dx, dy,
                                                      NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_pointer_motion (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_pointer_motion_absolute (XdpRemoteDesktop *object,
                                       GDBusMethodInvocation *invocation,
                                       const char *arg_session_handle,
                                       GVariant *arg_options,
                                       uint32_t stream,
                                       double x,
                                       double y)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_POINTER))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: pointer");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!check_position (session, stream, x, y))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid position");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_pointer_motion_absolute (impl,
                                                               session->id,
                                                               options,
                                                               stream,
                                                               x, y,
                                                               NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_pointer_motion_absolute (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_pointer_button (XdpRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              int32_t button,
                              uint32_t state)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_POINTER))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: pointer");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_pointer_button (impl,
                                                      session->id,
                                                      options,
                                                      button,
                                                      state,
                                                      NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_pointer_button (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey remote_desktop_notify_pointer_axis_options[] = {
  { "finish", G_VARIANT_TYPE_BOOLEAN, NULL },
};

static gboolean
handle_notify_pointer_axis (XdpRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            double dx,
                            double dy)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_POINTER))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: pointer");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_pointer_axis_options,
                           G_N_ELEMENTS (remote_desktop_notify_pointer_axis_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_pointer_axis (impl,
                                                    session->id,
                                                    options,
                                                    dx, dy,
                                                    NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_pointer_axis (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_pointer_axis_discrete (XdpRemoteDesktop *object,
                                     GDBusMethodInvocation *invocation,
                                     const char *arg_session_handle,
                                     GVariant *arg_options,
                                     uint32_t axis,
                                     int32_t steps)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_POINTER))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: pointer");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_pointer_axis_discrete (impl,
                                                             session->id,
                                                             options,
                                                             axis,
                                                             steps,
                                                             NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_pointer_axis_discrete (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_keyboard_keycode (XdpRemoteDesktop *object,
                                GDBusMethodInvocation *invocation,
                                const char *arg_session_handle,
                                GVariant *arg_options,
                                int32_t keycode,
                                uint32_t state)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_KEYBOARD))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: keyboard");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_keyboard_keycode (impl,
                                                        session->id,
                                                        options,
                                                        keycode,
                                                        state,
                                                        NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_keyboard_keycode (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_keyboard_keysym (XdpRemoteDesktop *object,
                               GDBusMethodInvocation *invocation,
                               const char *arg_session_handle,
                               GVariant *arg_options,
                               int32_t keysym,
                               uint32_t state)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_KEYBOARD))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: keyboard");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_keyboard_keysym (impl,
                                                       session->id,
                                                       options,
                                                       keysym,
                                                       state,
                                                       NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_keyboard_keysym (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_touch_down (XdpRemoteDesktop *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_session_handle,
                          GVariant *arg_options,
                          uint32_t stream,
                          uint32_t slot,
                          double x,
                          double y)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_TOUCHSCREEN))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: touchscreen");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!check_position (session, stream, x, y))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid position");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_touch_down (impl,
                                                  session->id,
                                                  options,
                                                  stream,
                                                  slot,
                                                  x, y,
                                                  NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_touch_down (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_touch_motion (XdpRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            uint32_t stream,
                            uint32_t slot,
                            double x,
                            double y)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_TOUCHSCREEN))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: touchscreen");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!check_position (session, stream, x, y))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid position");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_touch_motion (impl,
                                                    session->id,
                                                    options,
                                                    stream,
                                                    slot,
                                                    x, y,
                                                    NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_touch_motion (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_touch_up (XdpRemoteDesktop *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_session_handle,
                        GVariant *arg_options,
                        uint32_t slot)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
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

  if (!check_notify (session, DEVICE_TYPE_TOUCHSCREEN))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session doesn't have access to a device of type: touchscreen");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  xdp_impl_remote_desktop_call_notify_touch_up (impl,
                                                session->id,
                                                options,
                                                slot,
                                                NULL, NULL, NULL);

  xdp_remote_desktop_complete_notify_touch_up (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
remote_desktop_iface_init (XdpRemoteDesktopIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_select_devices = handle_select_devices;
  iface->handle_start = handle_start;

  iface->handle_notify_pointer_motion = handle_notify_pointer_motion;
  iface->handle_notify_pointer_motion_absolute = handle_notify_pointer_motion_absolute;
  iface->handle_notify_pointer_button = handle_notify_pointer_button;
  iface->handle_notify_pointer_axis = handle_notify_pointer_axis;
  iface->handle_notify_pointer_axis_discrete = handle_notify_pointer_axis_discrete;
  iface->handle_notify_keyboard_keycode = handle_notify_keyboard_keycode;
  iface->handle_notify_keyboard_keysym = handle_notify_keyboard_keysym;
  iface->handle_notify_touch_down = handle_notify_touch_down;
  iface->handle_notify_touch_motion = handle_notify_touch_motion;
  iface->handle_notify_touch_up = handle_notify_touch_up;
}

static void
sync_supported_device_types (RemoteDesktop *remote_desktop)
{
  unsigned int available_device_types;

  available_device_types =
    xdp_impl_remote_desktop_get_available_device_types (impl);
  xdp_remote_desktop_set_available_device_types (XDP_REMOTE_DESKTOP (remote_desktop),
                                                 available_device_types);
}

static void
on_supported_device_types_changed (GObject *gobject,
                                   GParamSpec *pspec,
                                   RemoteDesktop *remote_desktop)
{
  sync_supported_device_types (remote_desktop);
}

static void
remote_desktop_init (RemoteDesktop *remote_desktop)
{
  xdp_remote_desktop_set_version (XDP_REMOTE_DESKTOP (remote_desktop), 1);

  g_signal_connect (impl, "notify::supported-device-types",
                    G_CALLBACK (on_supported_device_types_changed),
                    remote_desktop);
  sync_supported_device_types (remote_desktop);
}

static void
remote_desktop_class_init (RemoteDesktopClass *klass)
{
}

GDBusInterfaceSkeleton *
remote_desktop_create (GDBusConnection *connection,
                       const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_remote_desktop_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 dbus_name,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create remote desktop proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  remote_desktop = g_object_new (remote_desktop_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (remote_desktop);
}

static void
remote_desktop_session_close (Session *session)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)session;

  remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_CLOSED;

  g_debug ("remote desktop session owned by '%s' closed", session->sender);
}

static void
remote_desktop_session_finalize (GObject *object)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)object;

  g_list_free_full (remote_desktop_session->streams,
                    (GDestroyNotify)screen_cast_stream_free);

  G_OBJECT_CLASS (remote_desktop_session_parent_class)->finalize (object);
}

static void
remote_desktop_session_init (RemoteDesktopSession *remote_desktop_session)
{
}

static void
remote_desktop_session_class_init (RemoteDesktopSessionClass *klass)
{
  GObjectClass *object_class;
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = remote_desktop_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = remote_desktop_session_close;

  quark_request_session =
    g_quark_from_static_string ("-xdp-request-remote-desktop-session");
}
