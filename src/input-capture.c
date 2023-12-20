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
#include <glib.h>
#include <gio/gunixfdlist.h>

#include "session.h"
#include "input-capture.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define VERSION_1  1 /* Makes grep easier */

typedef struct _InputCapture InputCapture;
typedef struct _InputCaptureClass InputCaptureClass;

struct _InputCapture
{
  XdpDbusInputCaptureSkeleton parent_instance;
};

struct _InputCaptureClass
{
  XdpDbusInputCaptureSkeletonClass parent_class;
};

static XdpDbusImplInputCapture *impl;
static int impl_version;
static InputCapture *input_capture;

static GQuark quark_request_session;

GType input_capture_get_type (void);
static void input_capture_iface_init (XdpDbusInputCaptureIface *iface);

G_DEFINE_TYPE_WITH_CODE (InputCapture, input_capture, XDP_DBUS_TYPE_INPUT_CAPTURE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_INPUT_CAPTURE,
                                                input_capture_iface_init))

typedef enum _InputCaptureSessionState
{
  INPUT_CAPTURE_SESSION_STATE_INIT,
  INPUT_CAPTURE_SESSION_STATE_ENABLED,
  INPUT_CAPTURE_SESSION_STATE_ACTIVE,
  INPUT_CAPTURE_SESSION_STATE_DISABLED,
  INPUT_CAPTURE_SESSION_STATE_CLOSED
} InputCaptureSessionState;

typedef struct _InputCaptureSession
{
  Session parent;

  InputCaptureSessionState state;
} InputCaptureSession;

typedef struct _InputCaptureSessionClass
{
  SessionClass parent_class;
} InputCaptureSessionClass;

GType input_capture_session_get_type (void);

G_DEFINE_TYPE (InputCaptureSession, input_capture_session, session_get_type ())

static gboolean
is_input_capture_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, input_capture_session_get_type ());
}

static InputCaptureSession *
input_capture_session_new (GVariant  *options,
                           Request   *request,
                           GError   **error)
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
  session = g_initable_new (input_capture_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("capture input session owned by '%s' created", session->sender);

  return (InputCaptureSession*)session;
}

static void
create_session_done (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      data)
{
  g_autoptr(Request) request = data;
  g_autoptr(GError) error = NULL;
  GVariantBuilder results_builder;
  GVariant *results;
  Session *session;
  gboolean should_close_session;
  uint32_t capabilities = 0;
  uint32_t response = 2;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_dbus_impl_input_capture_call_create_session_finish (impl,
                                                               &response,
                                                               &results,
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

      if (!g_variant_lookup (results, "capabilities", "u", &capabilities))
        {
          g_warning ("Impl did not set capabilities");
          response = 2;
          should_close_session = TRUE;
          goto out;
        }

      should_close_session = FALSE;
      session_register (session);

      g_variant_builder_add (&results_builder, "{sv}",
                            "capabilities", g_variant_new_uint32 (capabilities));
      g_variant_builder_add (&results_builder, "{sv}",
                             "session_handle", g_variant_new ("o", session->id));
    }
  else
    {
      should_close_session = TRUE;
    }

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
validate_capabilities (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       GError     **error)
{
  uint32_t types = g_variant_get_uint32 (value);

  if ((types & ~(1 | 2 | 4 | 8)) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unsupported capability: %x", types & ~(1 | 2 | 4 | 8));
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey input_capture_create_session_options[] = {
  { "capabilities", G_VARIANT_TYPE_UINT32, validate_capabilities },
};

static gboolean
handle_create_session (XdpDbusInputCapture   *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_parent_window,
                       GVariant              *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  GVariant *options;
  Session *session;

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

  session = (Session *)input_capture_session_new (arg_options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           input_capture_create_session_options,
                           G_N_ELEMENTS (input_capture_create_session_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_input_capture_call_create_session (impl,
                                                   request->id,
                                                   session->id,
                                                   xdp_app_info_get_id (request->app_info),
                                                   arg_parent_window,
                                                   options,
                                                   NULL,
                                                   create_session_done,
                                                   g_object_ref (request));

  xdp_dbus_input_capture_complete_create_session (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
get_zones_done (GObject      *source_object,
                GAsyncResult *res,
                gpointer      data)
{
  g_autoptr(GVariant) results = NULL;
  g_autoptr(Request) request = NULL;
  g_autoptr(GError) error = NULL;
  Session *session;
  gboolean should_close_session;
  uint32_t response = 2;

  request = (Request *)data;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_dbus_impl_input_capture_call_get_zones_finish (impl,
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
      if (response != 0)
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
}

static XdpOptionKey input_capture_get_zones_options[] = {
};

static gboolean
handle_get_zones (XdpDbusInputCapture   *object,
                  GDBusMethodInvocation *invocation,
                  const char            *arg_session_handle,
                  GVariant              *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  InputCaptureSession *input_capture_session;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  GVariant *options;
  Session *session;

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

  if (!is_input_capture_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  input_capture_session = (InputCaptureSession *)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
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
                           input_capture_get_zones_options,
                           G_N_ELEMENTS (input_capture_get_zones_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_input_capture_call_get_zones (impl,
                                              request->id,
                                              arg_session_handle,
                                              xdp_app_info_get_id (request->app_info),
                                              options,
                                              NULL,
                                              get_zones_done,
                                              g_object_ref (request));

  xdp_dbus_input_capture_complete_get_zones (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
set_pointer_barriers_done (GObject      *source_object,
                           GAsyncResult *res,
                           gpointer      data)
{
  g_autoptr(GVariant) results = NULL;
  g_autoptr(Request) request = NULL;
  g_autoptr(GError) error = NULL;
  gboolean should_close_session;
  Session *session;
  uint32_t response = 2;

  request = (Request *)data;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_dbus_impl_input_capture_call_set_pointer_barriers_finish (impl,
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
      if (response != 0)
        {
          GVariantBuilder results_builder;

          g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
          results = g_variant_builder_end (&results_builder);
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      request_unexport (request);
    }

  if (should_close_session)
    session_close (session, TRUE);
}

static XdpOptionKey input_capture_set_pointer_barriers_options[] = {
};

static gboolean
handle_set_pointer_barriers (XdpDbusInputCapture   *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_session_handle,
                             GVariant              *arg_options,
                             GVariant              *arg_barriers,
                             uint32_t               arg_zone_set)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  InputCaptureSession *input_capture_session;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  GVariant *options;
  Session *session;

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

  if (!is_input_capture_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  input_capture_session = (InputCaptureSession *)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
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
                           input_capture_set_pointer_barriers_options,
                           G_N_ELEMENTS (input_capture_set_pointer_barriers_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_input_capture_call_set_pointer_barriers (impl,
                                                         request->id,
                                                         arg_session_handle,
                                                         xdp_app_info_get_id (request->app_info),
                                                         options,
                                                         g_variant_ref (arg_barriers), /* FIXME: validation? */
                                                         arg_zone_set, /* FIXME: validation? */
                                                         NULL,
                                                         set_pointer_barriers_done,
                                                         g_object_ref (request));

  xdp_dbus_input_capture_complete_set_pointer_barriers (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey input_capture_enable_options[] = {
};

static gboolean
handle_enable (XdpDbusInputCapture   *object,
               GDBusMethodInvocation *invocation,
               const char            *arg_session_handle,
               GVariant              *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  InputCaptureSession *input_capture_session;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;

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

  if (!is_input_capture_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

   input_capture_session = (InputCaptureSession *)session;

   switch (input_capture_session->state)
     {
       case INPUT_CAPTURE_SESSION_STATE_INIT:
         g_dbus_method_invocation_return_error (invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Not connected to EIS");
         return G_DBUS_METHOD_INVOCATION_HANDLED;
       case INPUT_CAPTURE_SESSION_STATE_ENABLED:
       case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
       case INPUT_CAPTURE_SESSION_STATE_DISABLED:
         break;
       case INPUT_CAPTURE_SESSION_STATE_CLOSED:
         g_dbus_method_invocation_return_error (invocation,
                                                G_DBUS_ERROR,
                                                G_DBUS_ERROR_FAILED,
                                                "Invalid session");
         return G_DBUS_METHOD_INVOCATION_HANDLED;
     }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_filter_options (arg_options, &options_builder,
                           input_capture_enable_options,
                           G_N_ELEMENTS (input_capture_enable_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;

  /* Let's be lenient and make Enable() a noop for anything but a disabled
   * session.
   */
  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT: /* ignore, handled above */
        g_assert_not_reached ();
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED: /* ignore, handled above */
        g_assert_not_reached ();
    }

  xdp_dbus_impl_input_capture_call_enable (impl,
                                           arg_session_handle,
                                           xdp_app_info_get_id (call->app_info),
					   g_variant_builder_end (&options_builder),
                                           NULL,
                                           NULL,
                                           NULL);

  xdp_dbus_input_capture_complete_enable (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey input_capture_disable_options[] = {
};

static gboolean
handle_disable (XdpDbusInputCapture   *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_session_handle,
                GVariant              *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  InputCaptureSession *input_capture_session;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;

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

  if (!is_input_capture_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

   input_capture_session = (InputCaptureSession *)session;

   switch (input_capture_session->state)
     {
       case INPUT_CAPTURE_SESSION_STATE_INIT:
         g_dbus_method_invocation_return_error (invocation,
                                                G_DBUS_ERROR,
                                                G_DBUS_ERROR_FAILED,
                                                "Not connected to EIS");
         return G_DBUS_METHOD_INVOCATION_HANDLED;
       case INPUT_CAPTURE_SESSION_STATE_ENABLED:
       case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
       case INPUT_CAPTURE_SESSION_STATE_DISABLED:
         break;
       case INPUT_CAPTURE_SESSION_STATE_CLOSED:
         g_dbus_method_invocation_return_error (invocation,
                                                G_DBUS_ERROR,
                                                G_DBUS_ERROR_FAILED,
                                                "Invalid session");
         return G_DBUS_METHOD_INVOCATION_HANDLED;
     }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           input_capture_disable_options,
                           G_N_ELEMENTS (input_capture_disable_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* We need to be lenient, a caller may call Disable() before processing a
   * Disabled signal. So we pretend everything's ok but only
   * update our internal state in the right transitions.
   */
  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT: /* ignore, handled above */
        g_assert_not_reached ();
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED: /* ignore, handled above */
        g_assert_not_reached ();
    }

  xdp_dbus_impl_input_capture_call_disable (impl,
                                            arg_session_handle,
                                            xdp_app_info_get_id (call->app_info),
                                            g_variant_builder_end (&options_builder),
                                            NULL,
                                            NULL,
                                            NULL);

  xdp_dbus_input_capture_complete_disable (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey input_capture_release_options[] = {
  { "cursor_position", (const GVariantType *)"(dd)", NULL },
  { "activation_id", G_VARIANT_TYPE_UINT32, NULL },
};

static gboolean
handle_release (XdpDbusInputCapture   *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_session_handle,
                GVariant              *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  InputCaptureSession *input_capture_session;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;

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

  if (!is_input_capture_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  input_capture_session = (InputCaptureSession *)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
        g_dbus_method_invocation_return_error (invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Not connected to EIS");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           input_capture_release_options,
                           G_N_ELEMENTS (input_capture_release_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* We need to be lenient, a caller may call Release() before processing a
   * Deactivated/Disabled signal. So we pretend everything's ok but only
   * update our internal state in the right transitions.
   */
  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT: /* ignore, handled above */
        g_assert_not_reached ();
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED: /* ignore, handled above */
        g_assert_not_reached ();
    }

  xdp_dbus_impl_input_capture_call_release (impl,
                                            arg_session_handle,
                                            xdp_app_info_get_id (call->app_info),
                                            g_variant_builder_end (&options_builder),
                                            NULL,
                                            NULL,
                                            NULL);

  xdp_dbus_input_capture_complete_release (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_connect_to_eis (XdpDbusInputCapture   *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList           *in_fd_list,
                       const char            *arg_session_handle,
                       GVariant              *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  InputCaptureSession *input_capture_session;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder empty;
  GVariant *fd;

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

  if (!is_input_capture_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  input_capture_session = (InputCaptureSession *)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
          break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Already connected");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Invalid session");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&empty, G_VARIANT_TYPE_VARDICT);

  if (!xdp_dbus_impl_input_capture_call_connect_to_eis_sync (impl,
                                                             arg_session_handle,
                                                             xdp_app_info_get_id (call->app_info),
                                                             g_variant_builder_end (&empty),
                                                             in_fd_list,
                                                             &fd,
                                                             &out_fd_list,
                                                             NULL,
                                                             &error))
    {
      g_warning ("Failed to ConnectToEIS: %s", error->message);
      out_fd_list = g_unix_fd_list_new ();
    }

  input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;

  xdp_dbus_input_capture_complete_connect_to_eis (object, invocation, out_fd_list, fd);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
input_capture_iface_init (XdpDbusInputCaptureIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_get_zones = handle_get_zones;
  iface->handle_set_pointer_barriers = handle_set_pointer_barriers;
  iface->handle_connect_to_eis = handle_connect_to_eis;
  iface->handle_enable = handle_enable;
  iface->handle_disable = handle_disable;
  iface->handle_release = handle_release;
}

static void
pass_signal (XdpDbusImplInputCapture *impl,
             const char              *signal_name,
             const char              *session_id,
             GVariant                *options,
             gpointer                 data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(Session) session = lookup_session (session_id);

  g_dbus_connection_emit_signal (connection,
                                 session->sender,
                                 "/org/freedesktop/portal/desktop",
                                 "org.freedesktop.portal.InputCapture",
                                 signal_name,
                                 g_variant_new ("(o@a{sv})", session_id, options),
                                 NULL);
}

static void
on_disabled_cb (XdpDbusImplInputCapture *impl,
                const char              *session_id,
                GVariant                *options,
                gpointer                 data)
{
  g_autoptr(Session) session = lookup_session (session_id);
  InputCaptureSession *input_capture_session;

  if (!is_input_capture_session (session))
    {
      g_critical ("Invalid session type for signal");
      return;
    }

  input_capture_session = (InputCaptureSession*)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        pass_signal (impl, "Disabled", session_id, options, data);
        input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        break;
    }
}

static void
on_activated_cb (XdpDbusImplInputCapture *impl,
                 const char              *session_id,
                 GVariant                *options,
                 gpointer                 data)
{
  g_autoptr(Session) session = lookup_session (session_id);
  InputCaptureSession *input_capture_session;

  if (!is_input_capture_session (session))
    {
      g_critical ("Invalid session type for signal");
      return;
    }

  input_capture_session = (InputCaptureSession*)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
        pass_signal (impl, "Activated", session_id, options, data);
        input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_ACTIVE;
        break;
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        break;
    }
}

static void
on_deactivated_cb (XdpDbusImplInputCapture *impl,
                   const char              *session_id,
                   GVariant                *options,
                   gpointer                 data)
{
  g_autoptr(Session) session = lookup_session (session_id);
  InputCaptureSession *input_capture_session;

  if (!is_input_capture_session (session))
    {
      g_critical ("Invalid session type for signal");
      return;
    }

  input_capture_session = (InputCaptureSession*)session;

  switch (input_capture_session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        pass_signal (impl, "Deactivated", session_id, options, data);
        input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_ACTIVE;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        break;
    }
}

static void
input_capture_init (InputCapture *input_capture)
{
  unsigned int supported_capabilities;

  xdp_dbus_input_capture_set_version (XDP_DBUS_INPUT_CAPTURE (input_capture), VERSION_1);

  supported_capabilities =
    xdp_dbus_impl_input_capture_get_supported_capabilities (impl);
  xdp_dbus_input_capture_set_supported_capabilities (XDP_DBUS_INPUT_CAPTURE (input_capture),
                                                     supported_capabilities);

  g_signal_connect (impl, "disabled", G_CALLBACK (on_disabled_cb), input_capture);
  g_signal_connect (impl, "activated", G_CALLBACK (on_activated_cb), input_capture);
  g_signal_connect (impl, "deactivated", G_CALLBACK (on_deactivated_cb), input_capture);
}

static void
input_capture_class_init (InputCaptureClass *klass)
{
  quark_request_session =
    g_quark_from_static_string ("-xdp-request-capture-input-session");
}

static void
input_capture_session_close (Session *session)
{
  InputCaptureSession *input_capture_session = (InputCaptureSession *)session;

  input_capture_session->state = INPUT_CAPTURE_SESSION_STATE_CLOSED;

  g_debug ("screen cast session owned by '%s' closed", session->sender);
}

static void
input_capture_session_finalize (GObject *object)
{
  G_OBJECT_CLASS (input_capture_session_parent_class)->finalize (object);
}

static void
input_capture_session_init (InputCaptureSession *input_capture_session)
{
}

static void
input_capture_session_class_init (InputCaptureSessionClass *klass)
{
  GObjectClass *object_class;
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = input_capture_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = input_capture_session_close;
}

GDBusInterfaceSkeleton *
input_capture_create (GDBusConnection *connection,
                      const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_input_capture_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create capture input proxy: %s", error->message);
      return NULL;
    }

  impl_version = xdp_dbus_impl_input_capture_get_version (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  input_capture = g_object_new (input_capture_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (input_capture);
}
