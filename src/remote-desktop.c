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

#include <gio/gunixfdlist.h>
#include <stdint.h>

#include "screen-cast.h"
#include "pipewire.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-session.h"
#include "xdp-session-persistence.h"
#include "xdp-utils.h"

#include "remote-desktop.h"

typedef struct _RemoteDesktop RemoteDesktop;
typedef struct _RemoteDesktopClass RemoteDesktopClass;

struct _RemoteDesktop
{
  XdpDbusRemoteDesktopSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplRemoteDesktop *impl;
};

struct _RemoteDesktopClass
{
  XdpDbusRemoteDesktopSkeletonClass parent_class;
};

GType remote_desktop_get_type (void) G_GNUC_CONST;
static void remote_desktop_iface_init (XdpDbusRemoteDesktopIface *iface);

static GQuark quark_request_session;

G_DEFINE_TYPE_WITH_CODE (RemoteDesktop, remote_desktop,
                         XDP_DBUS_TYPE_REMOTE_DESKTOP_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REMOTE_DESKTOP,
                                                remote_desktop_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RemoteDesktop, g_object_unref)

typedef enum _RemoteDesktopSessionState
{
  REMOTE_DESKTOP_SESSION_STATE_INIT,
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
  XdpSession parent;

  RemoteDesktop *remote_desktop;

  RemoteDesktopSessionState state;

  DeviceType shared_devices;

  GList *streams;

  gboolean clipboard_requested;

  gboolean devices_selected;

  gboolean sources_selected;

  gboolean clipboard_enabled;

  gboolean uses_eis;

  char *restore_token;
  XdpSessionPersistenceMode persist_mode;
  GVariant *restore_data;
} RemoteDesktopSession;

typedef struct _RemoteDesktopSessionClass
{
  XdpSessionClass parent_class;
} RemoteDesktopSessionClass;

G_DEFINE_TYPE (RemoteDesktopSession, remote_desktop_session, xdp_session_get_type ())

gboolean
remote_desktop_session_can_select_sources (RemoteDesktopSession *session)
{
  if (session->sources_selected)
    return FALSE;

  switch (session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
      return TRUE;
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      return FALSE;
    }

  g_assert_not_reached ();
}

gboolean
remote_desktop_session_can_select_devices (RemoteDesktopSession *session)
{
  if (session->devices_selected)
    return FALSE;

  switch (session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
      return TRUE;
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      return FALSE;
    }

  g_assert_not_reached ();
}

gboolean
remote_desktop_session_can_request_clipboard (RemoteDesktopSession *session)
{
  RemoteDesktop *remote_desktop = session->remote_desktop;

  if (session->clipboard_requested)
    return FALSE;

  if (xdp_dbus_impl_remote_desktop_get_version (remote_desktop->impl) < 2)
    return FALSE;

  switch (session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
      return TRUE;
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
remote_desktop_session_sources_selected (RemoteDesktopSession *session)
{
  session->sources_selected = TRUE;
}

gboolean
remote_desktop_session_is_clipboard_enabled (RemoteDesktopSession *session)
{
  return session->clipboard_enabled;
}

void
remote_desktop_session_clipboard_requested (RemoteDesktopSession *session)
{
  session->clipboard_requested = TRUE;
}

static void
remote_desktop_session_close (XdpSession *session)
{
  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_CLOSED;

  g_debug ("remote desktop session owned by '%s' closed", session->sender);
}

static void
remote_desktop_session_finalize (GObject *object)
{
  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (object);

  g_list_free_full (remote_desktop_session->streams,
                    (GDestroyNotify)screen_cast_stream_free);

  g_clear_object (&remote_desktop_session->remote_desktop);

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
  XdpSessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = remote_desktop_session_finalize;

  session_class = (XdpSessionClass *)klass;
  session_class->close = remote_desktop_session_close;

  quark_request_session =
    g_quark_from_static_string ("-xdp-request-remote-desktop-session");
}

static RemoteDesktopSession *
remote_desktop_session_new (RemoteDesktop  *remote_desktop,
                            GVariant       *options,
                            XdpRequest     *request,
                            GError        **error)
{
  XdpSession *session;
  RemoteDesktopSession *rd_session;
  GDBusInterfaceSkeleton *interface_skeleton =
    G_DBUS_INTERFACE_SKELETON (request);
  const char *session_token;
  GDBusConnection *connection =
    g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (remote_desktop->impl));
  const char *impl_dbus_name =
    g_dbus_proxy_get_name (G_DBUS_PROXY (remote_desktop->impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (remote_desktop_session_get_type (), NULL, error,
                            "context", remote_desktop->context,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);
  if (session == NULL)
    return NULL;

  rd_session = REMOTE_DESKTOP_SESSION (session);
  rd_session->remote_desktop = g_object_ref (remote_desktop);

  g_debug ("remote desktop session owned by '%s' created", session->sender);

  return rd_session;
}

static void
create_session_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  XdpDbusImplRemoteDesktop *impl = (XdpDbusImplRemoteDesktop *) source_object;
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

  if (!xdp_dbus_impl_remote_desktop_call_create_session_finish (impl,
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

static gboolean
handle_create_session (XdpDbusRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (remote_desktop->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (remote_desktop->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = XDP_SESSION (remote_desktop_session_new (remote_desktop, arg_options, request, &error));
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_remote_desktop_call_create_session (remote_desktop->impl,
                                                    request->id,
                                                    session->id,
                                                    xdp_app_info_get_id (request->app_info),
                                                    options,
                                                    NULL,
                                                    create_session_done,
                                                    g_object_ref (request));

  xdp_dbus_remote_desktop_complete_create_session (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
select_devices_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  XdpDbusImplRemoteDesktop *impl = (XdpDbusImplRemoteDesktop *) source_object;
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

  if (!xdp_dbus_impl_remote_desktop_call_select_devices_finish (impl,
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

  if (request->exported)
    {
      if (!results)
        {
          g_auto(GVariantBuilder) results_builder =
            G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response, results);
      xdp_request_unexport (request);
    }

  if (should_close_session)
    {
      xdp_session_close (session, TRUE);
    }
  else if (!session->closed)
    {
      RemoteDesktopSession *remote_desktop_session =
        REMOTE_DESKTOP_SESSION (session);

      remote_desktop_session->devices_selected = TRUE;
    }
}

static gboolean
validate_device_types (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       gpointer     user_data,
                       GError     **error)
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
validate_restore_token (const char  *key,
                        GVariant    *value,
                        GVariant    *options,
                        gpointer     user_data,
                        GError     **error)
{
  const char *restore_token = g_variant_get_string (value, NULL);

  if (!g_uuid_string_is_valid (restore_token))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Restore token is not a valid UUID string");
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_persist_mode (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       gpointer     user_data,
                       GError     **error)
{
  uint32_t mode = g_variant_get_uint32 (value);

  if (mode > XDP_SESSION_PERSISTENCE_MODE_PERSISTENT)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid persist mode %x", mode);
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey remote_desktop_select_devices_options[] = {
  { "types", G_VARIANT_TYPE_UINT32, validate_device_types },
  { "restore_token", G_VARIANT_TYPE_STRING, validate_restore_token },
  { "persist_mode", G_VARIANT_TYPE_UINT32, validate_persist_mode },
};

static gboolean
replace_remote_desktop_restore_token_with_data (XdpSession *session,
                                                GVariant **in_out_options,
                                                GError **error)
{
  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (session);
  g_autoptr(GVariant) options = NULL;
  XdpSessionPersistenceMode persist_mode;

  options = *in_out_options;

  if (!g_variant_lookup (options, "persist_mode", "u", &persist_mode))
    persist_mode = XDP_SESSION_PERSISTENCE_MODE_NONE;

  remote_desktop_session->persist_mode = persist_mode;
  xdp_session_persistence_replace_restore_token_with_data (session,
                                                           REMOTE_DESKTOP_PERMISSION_TABLE,
                                                           in_out_options,
                                                           &remote_desktop_session->restore_token);

  return TRUE;
}

static gboolean
handle_select_devices (XdpDbusRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  RemoteDesktopSession *remote_desktop_session;
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

  remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  if (!remote_desktop_session_can_select_devices (remote_desktop_session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_FAILED,
                                              "Invalid state");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (remote_desktop->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (remote_desktop->impl)),
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
                           remote_desktop_select_devices_options,
                           G_N_ELEMENTS (remote_desktop_select_devices_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  /* If 'restore_token' is passed, lookup the corresponding data in the
   * permission store and / or the GHashTable with transient permissions.
   * Portal implementations do not have access to the restore token.
   */
  if (!replace_remote_desktop_restore_token_with_data (session, &options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_remote_desktop_call_select_devices (remote_desktop->impl,
                                                    request->id,
                                                    arg_session_handle,
                                                    xdp_app_info_get_id (request->app_info),
                                                    options,
                                                    NULL,
                                                    select_devices_done,
                                                    g_object_ref (request));

  xdp_dbus_remote_desktop_complete_select_devices (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
replace_restore_remote_desktop_data_with_token (RemoteDesktopSession *remote_desktop_session,
                                                GVariant **in_out_results)
{
  xdp_session_persistence_replace_restore_data_with_token (XDP_SESSION (remote_desktop_session),
                                                           REMOTE_DESKTOP_PERMISSION_TABLE,
                                                           in_out_results,
                                                           &remote_desktop_session->persist_mode,
                                                           &remote_desktop_session->restore_token,
                                                           &remote_desktop_session->restore_data);
}

static gboolean
process_results (RemoteDesktopSession *remote_desktop_session,
                 GVariant **in_out_results,
                 GError **error)
{
  g_autoptr(GVariantIter) streams_iter = NULL;
  GVariant *results = *in_out_results;
  uint32_t devices = 0;
  gboolean clipboard_enabled = FALSE;

  if (g_variant_lookup (results, "streams", "a(ua{sv})", &streams_iter))
    {
      remote_desktop_session->streams =
        collect_screen_cast_stream_data (streams_iter);
    }

  if (g_variant_lookup (results, "devices", "u", &devices))
    remote_desktop_session->shared_devices = devices;

  if (g_variant_lookup (results, "clipboard_enabled", "b", &clipboard_enabled))
    remote_desktop_session->clipboard_enabled = clipboard_enabled;

  replace_restore_remote_desktop_data_with_token (remote_desktop_session,
                                                  in_out_results);

  return TRUE;
}

static void
start_done (GObject *source_object,
            GAsyncResult *res,
            gpointer data)
{
  XdpDbusImplRemoteDesktop *impl = (XdpDbusImplRemoteDesktop *) source_object;
  g_autoptr(XdpRequest) request = data;
  XdpSession *session;
  RemoteDesktopSession *remote_desktop_session;
  guint response = 2;
  gboolean should_close_session;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  remote_desktop_session = REMOTE_DESKTOP_SESSION (session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_dbus_impl_remote_desktop_call_start_finish (impl,
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

  if (request->exported)
    {
      if (response == 0)
        {
          if (!process_results (remote_desktop_session, &results, &error))
            {
              g_warning ("Could not start remote desktop session: %s",
                         error->message);
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

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response, results);
      xdp_request_unexport (request);
    }

  if (should_close_session)
    {
      xdp_session_close (session, TRUE);
    }
  else if (!session->closed)
    {
      g_debug ("remote desktop session owned by '%s' started", session->sender);
      remote_desktop_session->state = REMOTE_DESKTOP_SESSION_STATE_STARTED;
    }
}

static gboolean
handle_start (XdpDbusRemoteDesktop *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  RemoteDesktopSession *remote_desktop_session;
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

  remote_desktop_session = REMOTE_DESKTOP_SESSION (session);
  switch (remote_desktop_session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
      break;
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

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (remote_desktop->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (remote_desktop->impl)),
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

  xdp_dbus_impl_remote_desktop_call_start (remote_desktop->impl,
                                           request->id,
                                           arg_session_handle,
                                           xdp_app_info_get_id (request->app_info),
                                           arg_parent_window,
                                           options,
                                           NULL,
                                           start_done,
                                           g_object_ref (request));

  xdp_dbus_remote_desktop_complete_start (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
check_notify (XdpSession *session,
              DeviceType device_type)
{
  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  if (!remote_desktop_session->devices_selected || remote_desktop_session->uses_eis)
    return FALSE;

  switch (remote_desktop_session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
      break;
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      return FALSE;
    }

  if ((remote_desktop_session->shared_devices & device_type) == 0)
    return FALSE;

  return TRUE;
}

static gboolean
check_position (XdpSession *session,
                uint32_t stream,
                double x,
                double y)
{
  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (session);
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
handle_notify_pointer_motion (XdpDbusRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              double dx,
                              double dy)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyPointer methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_pointer_motion (remote_desktop->impl,
                                                           session->id,
                                                           options,
                                                           dx, dy,
                                                           NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_pointer_motion (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_pointer_motion_absolute (XdpDbusRemoteDesktop *object,
                                       GDBusMethodInvocation *invocation,
                                       const char *arg_session_handle,
                                       GVariant *arg_options,
                                       uint32_t stream,
                                       double x,
                                       double y)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyPointer methods");
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

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_pointer_motion_absolute (remote_desktop->impl,
                                                                    session->id,
                                                                    options,
                                                                    stream,
                                                                    x, y,
                                                                    NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_pointer_motion_absolute (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_pointer_button (XdpDbusRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              int32_t button,
                              uint32_t state)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyPointer methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_pointer_button (remote_desktop->impl,
                                                           session->id,
                                                           options,
                                                           button,
                                                           state,
                                                           NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_pointer_button (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey remote_desktop_notify_pointer_axis_options[] = {
  { "finish", G_VARIANT_TYPE_BOOLEAN, NULL },
};

static gboolean
handle_notify_pointer_axis (XdpDbusRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            double dx,
                            double dy)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyPointer methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_pointer_axis_options,
                           G_N_ELEMENTS (remote_desktop_notify_pointer_axis_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_pointer_axis (remote_desktop->impl,
                                                         session->id,
                                                         options,
                                                         dx, dy,
                                                         NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_pointer_axis (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_pointer_axis_discrete (XdpDbusRemoteDesktop *object,
                                     GDBusMethodInvocation *invocation,
                                     const char *arg_session_handle,
                                     GVariant *arg_options,
                                     uint32_t axis,
                                     int32_t steps)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyPointer methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_pointer_axis_discrete (remote_desktop->impl,
                                                                  session->id,
                                                                  options,
                                                                  axis,
                                                                  steps,
                                                                  NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_pointer_axis_discrete (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_keyboard_keycode (XdpDbusRemoteDesktop *object,
                                GDBusMethodInvocation *invocation,
                                const char *arg_session_handle,
                                GVariant *arg_options,
                                int32_t keycode,
                                uint32_t state)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyPointer methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_keyboard_keycode (remote_desktop->impl,
                                                             session->id,
                                                             options,
                                                             keycode,
                                                             state,
                                                             NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_keyboard_keycode (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_keyboard_keysym (XdpDbusRemoteDesktop *object,
                               GDBusMethodInvocation *invocation,
                               const char *arg_session_handle,
                               GVariant *arg_options,
                               int32_t keysym,
                               uint32_t state)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyKeyboard methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_keyboard_keysym (remote_desktop->impl,
                                                            session->id,
                                                            options,
                                                            keysym,
                                                            state,
                                                            NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_keyboard_keysym (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_touch_down (XdpDbusRemoteDesktop *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_session_handle,
                          GVariant *arg_options,
                          uint32_t stream,
                          uint32_t slot,
                          double x,
                          double y)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyTouch methods");
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

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_touch_down (remote_desktop->impl,
                                                       session->id,
                                                       options,
                                                       stream,
                                                       slot,
                                                       x, y,
                                                       NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_touch_down (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_touch_motion (XdpDbusRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            uint32_t stream,
                            uint32_t slot,
                            double x,
                            double y)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyTouch methods");
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

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_touch_motion (remote_desktop->impl,
                                                         session->id,
                                                         options,
                                                         stream,
                                                         slot,
                                                         x, y,
                                                         NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_touch_motion (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_notify_touch_up (XdpDbusRemoteDesktop *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_session_handle,
                        GVariant *arg_options,
                        uint32_t slot)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
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
                                             "Session is not allowed to call NotifyTouch methods");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_notify_options,
                           G_N_ELEMENTS (remote_desktop_notify_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_remote_desktop_call_notify_touch_up (remote_desktop->impl,
                                                     session->id,
                                                     options,
                                                     slot,
                                                     NULL, NULL, NULL);

  xdp_dbus_remote_desktop_complete_notify_touch_up (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey remote_desktop_connect_to_eis_options[] = {
};

static gboolean
handle_connect_to_eis (XdpDbusRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList *in_fd_list,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSession *session;
  RemoteDesktopSession *remote_desktop_session;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) fd = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!IS_REMOTE_DESKTOP_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  if (remote_desktop_session->uses_eis)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session is already connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (remote_desktop_session->state)
    {
    case REMOTE_DESKTOP_SESSION_STATE_STARTED:
      break;
    case REMOTE_DESKTOP_SESSION_STATE_INIT:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session is not ready");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case REMOTE_DESKTOP_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session is already closed");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options_builder,
                           remote_desktop_connect_to_eis_options,
                           G_N_ELEMENTS (remote_desktop_connect_to_eis_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_dbus_impl_remote_desktop_call_connect_to_eis_sync (remote_desktop->impl,
                                                             arg_session_handle,
                                                             xdp_app_info_get_id (app_info),
                                                             g_variant_builder_end (&options_builder),
                                                             in_fd_list,
                                                             &fd,
                                                             &out_fd_list,
                                                             NULL,
                                                             &error))
    {
      g_warning ("Failed to ConnectToEIS: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  remote_desktop_session->uses_eis = TRUE;

  xdp_dbus_remote_desktop_complete_connect_to_eis (object, invocation, out_fd_list, fd);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
remote_desktop_iface_init (XdpDbusRemoteDesktopIface *iface)
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

  iface->handle_connect_to_eis = handle_connect_to_eis;
}

static void
remote_desktop_dispose (GObject *object)
{
  RemoteDesktop *remote_desktop = (RemoteDesktop *) object;

  g_clear_object (&remote_desktop->impl);

  G_OBJECT_CLASS (remote_desktop_parent_class)->dispose (object);
}


static void
remote_desktop_init (RemoteDesktop *remote_desktop)
{
}

static void
remote_desktop_class_init (RemoteDesktopClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = remote_desktop_dispose;
}

static RemoteDesktop *
remote_desktop_new (XdpContext               *context,
                    XdpDbusImplRemoteDesktop *impl)
{
  RemoteDesktop *remote_desktop;

  remote_desktop = g_object_new (remote_desktop_get_type (), NULL);
  remote_desktop->context = context;
  remote_desktop->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (remote_desktop->impl), G_MAXINT);

  xdp_dbus_remote_desktop_set_version (XDP_DBUS_REMOTE_DESKTOP (remote_desktop), 2);

  g_object_bind_property (G_OBJECT (remote_desktop->impl), "available-device-types",
                          G_OBJECT (remote_desktop), "available-device-types",
                          G_BINDING_SYNC_CREATE);

  return remote_desktop;
}

void
init_remote_desktop (XdpContext *context)
{
  g_autoptr(RemoteDesktop) remote_desktop = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplRemoteDesktop) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, REMOTE_DESKTOP_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_remote_desktop_proxy_new_sync (connection,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      impl_config->dbus_name,
                                                      DESKTOP_DBUS_PATH,
                                                      NULL,
                                                      &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create remote desktop proxy: %s", error->message);
      return;
    }

  remote_desktop = remote_desktop_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&remote_desktop)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
