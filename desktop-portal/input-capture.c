/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "input-capture.h"

#include <stdint.h>

#include <gio/gunixfdlist.h>
#include <glib.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request-dex.h"
#include "xdp-session-dex.h"
#include "xdp-session-persistence.h"
#include "xdp-utils.h"

#if HAVE_VARLINK
#include "xdp-varlink.h"
#include "xdp-varlink-session.h"
#endif

#define XDP_TYPE_INPUT_CAPTURE (xdp_input_capture_get_type ())
G_DECLARE_FINAL_TYPE (XdpInputCapture,
                      xdp_input_capture,
                      XDP, INPUT_CAPTURE,
                      XdpDbusInputCaptureSkeleton)

struct _XdpInputCapture
{
  XdpDbusInputCaptureSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplInputCapture *impl;
  int impl_version;
  XdpSessionDexStore *sessions;
};

static void xdp_input_capture_iface_init (XdpDbusInputCaptureIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpInputCapture,
                               xdp_input_capture,
                               XDP_DBUS_TYPE_INPUT_CAPTURE_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_INPUT_CAPTURE,
                                                      xdp_input_capture_iface_init))

typedef enum _InputCaptureCapabilities
{
  INPUT_CAPTURE_CAPABILITIES_KEYBOARD =    (1 << 0),
  INPUT_CAPTURE_CAPABILITIES_POINTER =     (1 << 1),
  INPUT_CAPTURE_CAPABILITIES_TOUCHSCREEN = (1 << 2),
  INPUT_CAPTURE_CAPABILITIES_ALL =         (1 << 3) - 1,
} InputCaptureCapabilities;

typedef enum _InputCaptureSessionState
{
  INPUT_CAPTURE_SESSION_STATE_INIT,
  INPUT_CAPTURE_SESSION_STATE_STARTED,
  INPUT_CAPTURE_SESSION_STATE_ENABLED,
  INPUT_CAPTURE_SESSION_STATE_ACTIVE,
  INPUT_CAPTURE_SESSION_STATE_DISABLED,
  INPUT_CAPTURE_SESSION_STATE_CLOSED
} InputCaptureSessionState;

struct _XdpInputCaptureSession
{
  GObject parent_instance;

  /* The member the session store indexes this wrapper by */
  XdpSessionDex *session;

  int impl_version;

  InputCaptureSessionState state;
  gboolean clipboard_requested;
  gboolean clipboard_enabled;

  char *restore_token;
  XdpSessionPersistenceMode persist_mode;
  GVariant *restore_data;

#if HAVE_VARLINK
  gboolean is_varlink;
  /* Each call is dropped when its connection closes */
  GPtrArray *capture_status_calls;
  GPtrArray *zones_calls;
#endif
};

G_DEFINE_FINAL_TYPE (XdpInputCaptureSession, xdp_input_capture_session, G_TYPE_OBJECT)

#if HAVE_VARLINK
static void
varlink_call_free (gpointer data)
{
  varlink_call_unref (data);
}

static void varlink_session_closed (XdpInputCaptureSession *session);
static void varlink_capture_status_changed (XdpInputCaptureSession *session,
                                            const char             *status,
                                            GVariant               *options);
static void varlink_zones_changed (XdpInputCapture        *input_capture,
                                   XdpInputCaptureSession *session);
#endif

static void
xdp_input_capture_session_dispose (GObject *object)
{
  XdpInputCaptureSession *self = XDP_INPUT_CAPTURE_SESSION (object);

  g_clear_object (&self->session);
  g_clear_pointer (&self->restore_token, g_free);
  g_clear_pointer (&self->restore_data, g_variant_unref);
#if HAVE_VARLINK
  g_clear_pointer (&self->capture_status_calls, g_ptr_array_unref);
  g_clear_pointer (&self->zones_calls, g_ptr_array_unref);
#endif

  G_OBJECT_CLASS (xdp_input_capture_session_parent_class)->dispose (object);
}

static void
xdp_input_capture_session_init (XdpInputCaptureSession *self)
{
}

static void
xdp_input_capture_session_class_init (XdpInputCaptureSessionClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = xdp_input_capture_session_dispose;
}

static void
on_session_closed (XdpSessionDex *session,
                   gpointer       user_data)
{
  XdpInputCaptureSession *self = XDP_INPUT_CAPTURE_SESSION (user_data);

  self->state = INPUT_CAPTURE_SESSION_STATE_CLOSED;

#if HAVE_VARLINK
  varlink_session_closed (self);
#endif
}

static XdpInputCaptureSession *
xdp_input_capture_session_new (XdpInputCapture *input_capture,
                               XdpSessionDex   *session)
{
  XdpInputCaptureSession *self;

  self = g_object_new (XDP_TYPE_INPUT_CAPTURE_SESSION, NULL);
  self->session = session;
  self->impl_version = input_capture->impl_version;
#if HAVE_VARLINK
  self->capture_status_calls =
    g_ptr_array_new_with_free_func (varlink_call_free);
  self->zones_calls =
    g_ptr_array_new_with_free_func (varlink_call_free);
#endif

  g_signal_connect_object (session, "session-closed",
                           G_CALLBACK (on_session_closed),
                           self,
                           G_CONNECT_DEFAULT);

  return self;
}

static XdpInputCaptureSession *
lookup_session (XdpInputCapture *input_capture,
                const char      *session_handle,
                XdpAppInfo      *app_info)
{
  return xdp_session_dex_store_lookup_session (input_capture->sessions,
                                               session_handle,
                                               app_info);
}

XdpInputCaptureSession *
input_capture_lookup_session (XdpContext *context,
                              const char *session_handle,
                              XdpAppInfo *app_info)
{
  XdpInputCaptureSession *session;
  GDBusInterfaceSkeleton *skeleton;

  skeleton = xdp_context_get_portal (context, INPUT_CAPTURE_DBUS_IFACE);
  if (skeleton == NULL)
    return NULL;

  session = lookup_session (XDP_INPUT_CAPTURE (skeleton), session_handle, app_info);

  return session ? g_object_ref (session) : NULL;
}

const char *
input_capture_session_get_object_path (XdpInputCaptureSession *session)
{
  return xdp_session_dex_get_object_path (session->session);
}

XdpAppInfo *
input_capture_session_get_app_info (XdpInputCaptureSession *session)
{
  return xdp_session_dex_get_app_info (session->session);
}

gboolean
input_capture_session_is_closed (XdpInputCaptureSession *session)
{
  return xdp_session_dex_is_closed (session->session);
}

gboolean
input_capture_session_can_request_clipboard (XdpInputCaptureSession *session)
{
  if (session->clipboard_requested)
    return FALSE;

  if (session->impl_version < 2)
    return FALSE;

  switch (session->state)
    {
    case INPUT_CAPTURE_SESSION_STATE_INIT:
      return TRUE;
    case INPUT_CAPTURE_SESSION_STATE_STARTED:
    case INPUT_CAPTURE_SESSION_STATE_ENABLED:
    case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
    case INPUT_CAPTURE_SESSION_STATE_DISABLED:
    case INPUT_CAPTURE_SESSION_STATE_CLOSED:
      return FALSE;
    }

  g_assert_not_reached ();
}

gboolean
input_capture_session_is_clipboard_enabled (XdpInputCaptureSession *session)
{
  return session->clipboard_enabled;
}

void
input_capture_session_clipboard_requested (XdpInputCaptureSession *session)
{
  session->clipboard_requested = TRUE;
}

gboolean
input_capture_session_can_access_clipboard (XdpInputCaptureSession *session)
{
  return session->clipboard_enabled &&
         session->state == INPUT_CAPTURE_SESSION_STATE_ACTIVE;
}

static gboolean
validate_capabilities (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       gpointer     user_data,
                       GError     **error)
{
  uint32_t types = g_variant_get_uint32 (value);

  if (types == 0 || (types & ~INPUT_CAPTURE_CAPABILITIES_ALL) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unsupported capability: %x", types & ~INPUT_CAPTURE_CAPABILITIES_ALL);
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
  return xdp_session_persistence_validate_restore_token (restore_token, error);
}

static gboolean
validate_persist_mode (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       gpointer     user_data,
                       GError     **error)
{
  return xdp_session_persistence_validate_persist_mode (g_variant_get_uint32 (value),
                                                        error);
}

static XdpOptionKey input_capture_create_session_options[] = {
  { "capabilities", G_VARIANT_TYPE_UINT32, validate_capabilities },
};

static XdpOptionKey input_capture_create_session2_options[] = {
};

static XdpOptionKey input_capture_start_options[] = {
  { "capabilities", G_VARIANT_TYPE_UINT32, validate_capabilities },
  { "restore_token", G_VARIANT_TYPE_STRING, validate_restore_token },
  { "persist_mode", G_VARIANT_TYPE_UINT32, validate_persist_mode },
};

static XdpOptionKey input_capture_get_zones_options[] = {
};

static XdpOptionKey input_capture_set_pointer_barriers_options[] = {
};

static XdpOptionKey input_capture_enable_options[] = {
};

static XdpOptionKey input_capture_disable_options[] = {
};

static XdpOptionKey input_capture_release_options[] = {
  { "cursor_position", (const GVariantType *)"(dd)", NULL },
  { "activation_id", G_VARIANT_TYPE_UINT32, NULL },
};

static GVariant *
filter_options (GVariant      *arg_options,
                XdpOptionKey  *supported,
                size_t         n_supported,
                GError       **error)
{
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!xdp_filter_options (arg_options, &options_builder,
                           supported, n_supported,
                           NULL, error))
    return NULL;

  return g_variant_ref_sink (g_variant_builder_end (&options_builder));
}

static void
replace_restore_token_with_data (XdpInputCaptureSession  *session,
                                 XdpAppInfo              *app_info,
                                 GVariant               **in_out_options)
{
  XdpSessionPersistenceMode persist_mode;

  if (!g_variant_lookup (*in_out_options, "persist_mode", "u", &persist_mode))
    persist_mode = XDP_SESSION_PERSISTENCE_MODE_NONE;

  session->persist_mode = persist_mode;
  xdp_session_persistence_replace_restore_token_with_data (app_info,
                                                           INPUT_CAPTURE_PERMISSION_TABLE,
                                                           in_out_options,
                                                           &session->restore_token);
}

/* Turns the backend's Start reply into the application's results, and says
 * whether the session may go on */
static gboolean
collect_start_results (XdpInputCaptureSession *session,
                       XdpAppInfo             *app_info,
                       GVariant               *results,
                       GVariantBuilder        *results_builder)
{
  g_autoptr(GVariant) owned_results = g_variant_ref (results);
  uint32_t capabilities = 0;
  gboolean clipboard_enabled = FALSE;

  session->state = INPUT_CAPTURE_SESSION_STATE_STARTED;

  g_variant_builder_add (results_builder, "{sv}",
                         "session_handle",
                         g_variant_new_object_path (input_capture_session_get_object_path (session)));

  if (!g_variant_lookup (results, "capabilities", "u", &capabilities))
    {
      g_warning ("Impl did not set capabilities");
      return FALSE;
    }

  g_variant_builder_add (results_builder, "{sv}",
                         "capabilities", g_variant_new_uint32 (capabilities));

  if (g_variant_lookup (results, "clipboard_enabled", "b", &clipboard_enabled))
    {
      session->clipboard_enabled = clipboard_enabled;

      g_debug ("Backend %s the clipboard for %s",
               clipboard_enabled ? "granted" : "refused",
               input_capture_session_get_object_path (session));

      g_variant_builder_add (results_builder, "{sv}",
                             "clipboard_enabled",
                             g_variant_new_boolean (clipboard_enabled));
    }
  else if (session->clipboard_requested)
    {
      g_debug ("Backend did not report clipboard_enabled for %s",
               input_capture_session_get_object_path (session));
    }

  xdp_session_persistence_replace_restore_data_with_token (app_info,
                                                           INPUT_CAPTURE_PERMISSION_TABLE,
                                                           &owned_results,
                                                           &session->persist_mode,
                                                           &session->restore_token,
                                                           &session->restore_data);
  if (session->restore_token)
    {
      g_variant_builder_add (results_builder, "{sv}",
                             "restore_token",
                             g_variant_new_string (session->restore_token));
    }

  return TRUE;
}

static gboolean
handle_create_session (XdpDbusInputCapture   *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_parent_window,
                       GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpInputCaptureSession) session = NULL;
  g_autoptr(XdpSessionDex) session_dex = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  uint32_t response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  gboolean keep_session;

  options = filter_options (arg_options,
                            input_capture_create_session_options,
                            G_N_ELEMENTS (input_capture_create_session_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_dex_new (input_capture->context,
                                                   app_info,
                                                   G_DBUS_INTERFACE_SKELETON (object),
                                                   G_DBUS_PROXY (input_capture->impl),
                                                   arg_options),
                              &error);
  if (!request || !xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session_dex = dex_await_object (xdp_session_dex_new (input_capture->context,
                                                       app_info,
                                                       G_DBUS_INTERFACE_SKELETON (object),
                                                       G_DBUS_PROXY (input_capture->impl),
                                                       arg_options),
                                  &error);
  if (!session_dex)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = xdp_input_capture_session_new (input_capture,
                                           g_steal_pointer (&session_dex));

  if (input_capture->impl_version >= 2)
    {
      g_auto(GVariantBuilder) empty_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autoptr(XdpDbusImplInputCaptureCreateSession2Result) create_result = NULL;

      create_result = dex_await_boxed (
        xdp_dbus_impl_input_capture_call_create_session2_future (
          input_capture->impl,
          input_capture_session_get_object_path (session),
          xdp_app_info_get_id (app_info),
          g_variant_builder_end (&empty_builder)),
        &error);

      if (!create_result)
        {
          xdp_session_dex_close (session->session, FALSE);
          g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  xdp_dbus_input_capture_complete_create_session (object,
                                                  g_steal_pointer (&invocation),
                                                  xdp_request_dex_get_object_path (request));
  G_GNUC_END_IGNORE_DEPRECATIONS

  if (input_capture->impl_version < 2)
    {
      g_autoptr(XdpDbusImplInputCaptureCreateSessionResult) result = NULL;

      result = dex_await_boxed (
        xdp_dbus_impl_input_capture_call_create_session_future (
          input_capture->impl,
          xdp_request_dex_get_object_path (request),
          input_capture_session_get_object_path (session),
          xdp_app_info_get_id (app_info),
          arg_parent_window,
          options),
        &error);

      if (result)
        {
          response = result->response;
          results = g_variant_ref (result->results);
        }
    }
  else
    {
      g_autoptr(XdpDbusImplInputCaptureStartResult) start_result = NULL;

      start_result = dex_await_boxed (
        xdp_dbus_impl_input_capture_call_start_future (
          input_capture->impl,
          xdp_request_dex_get_object_path (request),
          input_capture_session_get_object_path (session),
          xdp_app_info_get_id (app_info),
          arg_parent_window,
          options),
        &error);

      if (start_result)
        {
          response = start_result->response;
          results = g_variant_ref (start_result->results);
        }
    }

  if (!results)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  keep_session = results && !xdp_request_dex_is_closed (request) &&
                 response == XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;

  if (keep_session &&
      !collect_start_results (session, app_info, results, &results_builder))
    {
      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      keep_session = FALSE;
    }

  if (keep_session)
    {
      xdp_session_dex_store_take_session (input_capture->sessions,
                                          g_steal_pointer (&session));
    }
  else
    {
      xdp_session_dex_close (session->session, FALSE);
    }

  xdp_request_dex_emit_response (request, response,
                                 g_variant_builder_end (&results_builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_create_session2 (XdpDbusInputCapture   *object,
                        GDBusMethodInvocation *invocation,
                        GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpInputCaptureSession) session = NULL;
  g_autoptr(XdpSessionDex) session_dex = NULL;
  g_autoptr(XdpDbusImplInputCaptureCreateSession2Result) result = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (input_capture->impl_version < 2)
    return G_DBUS_METHOD_INVOCATION_UNHANDLED;

  options = filter_options (arg_options,
                            input_capture_create_session2_options,
                            G_N_ELEMENTS (input_capture_create_session2_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session_dex = dex_await_object (xdp_session_dex_new (input_capture->context,
                                                       app_info,
                                                       G_DBUS_INTERFACE_SKELETON (object),
                                                       G_DBUS_PROXY (input_capture->impl),
                                                       arg_options),
                                  &error);
  if (!session_dex)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = xdp_input_capture_session_new (input_capture,
                                           g_steal_pointer (&session_dex));

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_create_session2_future (
      input_capture->impl,
      input_capture_session_get_object_path (session),
      xdp_app_info_get_id (app_info),
      options),
    &error);

  if (!result)
    {
      xdp_session_dex_close (session->session, FALSE);
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_add (&results_builder, "{sv}",
                         "session_handle",
                         g_variant_new_object_path (input_capture_session_get_object_path (session)));

  xdp_session_dex_store_take_session (input_capture->sessions,
                                      g_steal_pointer (&session));

  xdp_dbus_input_capture_complete_create_session2 (object,
                                                   g_steal_pointer (&invocation),
                                                   g_variant_builder_end (&results_builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_start (XdpDbusInputCapture   *object,
              GDBusMethodInvocation *invocation,
              const char            *arg_session_handle,
              const char            *arg_parent_window,
              GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplInputCaptureStartResult) result = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  uint32_t response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;

  if (input_capture->impl_version < 2)
    return G_DBUS_METHOD_INVOCATION_UNHANDLED;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
        break;
    }

  options = filter_options (arg_options,
                            input_capture_start_options,
                            G_N_ELEMENTS (input_capture_start_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_dex_new (input_capture->context,
                                                   app_info,
                                                   G_DBUS_INTERFACE_SKELETON (object),
                                                   G_DBUS_PROXY (input_capture->impl),
                                                   arg_options),
                              &error);
  if (!request || !xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* Implementations never see the restore token, so it is swapped for the
   * data it stands for */
  replace_restore_token_with_data (session, app_info, &options);

  xdp_dbus_input_capture_complete_start (object,
                                         g_steal_pointer (&invocation),
                                         xdp_request_dex_get_object_path (request));

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_start_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      arg_session_handle,
      xdp_app_info_get_id (app_info),
      arg_parent_window,
      options),
    &error);

  if (!result)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }
  else
    {
      response = result->response;
    }

  if (result && !xdp_request_dex_is_closed (request) &&
      response == XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    {
      if (!collect_start_results (session, app_info, result->results, &results_builder))
        {
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
          xdp_session_dex_close (session->session, FALSE);
        }
    }
  else
    {
      xdp_session_dex_close (session->session, FALSE);
    }

  xdp_request_dex_emit_response (request, response,
                                 g_variant_builder_end (&results_builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_zones (XdpDbusInputCapture   *object,
                  GDBusMethodInvocation *invocation,
                  const char            *arg_session_handle,
                  GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplInputCaptureGetZonesResult) result = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  uint32_t response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = filter_options (arg_options,
                            input_capture_get_zones_options,
                            G_N_ELEMENTS (input_capture_get_zones_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_dex_new (input_capture->context,
                                                   app_info,
                                                   G_DBUS_INTERFACE_SKELETON (object),
                                                   G_DBUS_PROXY (input_capture->impl),
                                                   arg_options),
                              &error);
  if (!request || !xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_input_capture_complete_get_zones (object,
                                             g_steal_pointer (&invocation),
                                             xdp_request_dex_get_object_path (request));

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_get_zones_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      arg_session_handle,
      xdp_app_info_get_id (app_info),
      options),
    &error);

  if (!result)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }
  else
    {
      response = result->response;
    }

  xdp_request_dex_emit_response (request, response,
                                 result ? result->results : NULL);

  if (xdp_request_dex_is_closed (request) ||
      response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    xdp_session_dex_close (session->session, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_pointer_barriers (XdpDbusInputCapture   *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_session_handle,
                             GVariant              *arg_options,
                             GVariant              *arg_barriers,
                             uint32_t               arg_zone_set)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplInputCaptureSetPointerBarriersResult) result = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  uint32_t response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = filter_options (arg_options,
                            input_capture_set_pointer_barriers_options,
                            G_N_ELEMENTS (input_capture_set_pointer_barriers_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_dex_new (input_capture->context,
                                                   app_info,
                                                   G_DBUS_INTERFACE_SKELETON (object),
                                                   G_DBUS_PROXY (input_capture->impl),
                                                   arg_options),
                              &error);
  if (!request || !xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_input_capture_complete_set_pointer_barriers (object,
                                                        g_steal_pointer (&invocation),
                                                        xdp_request_dex_get_object_path (request));

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_set_pointer_barriers_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      arg_session_handle,
      xdp_app_info_get_id (app_info),
      options,
      arg_barriers,
      arg_zone_set),
    &error);

  if (!result)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }
  else
    {
      response = result->response;
    }

  xdp_request_dex_emit_response (request, response,
                                 result ? result->results : NULL);

  if (xdp_request_dex_is_closed (request) ||
      response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    xdp_session_dex_close (session->session, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_enable (XdpDbusInputCapture   *object,
               GDBusMethodInvocation *invocation,
               const char            *arg_session_handle,
               GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Not connected to EIS");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = filter_options (arg_options,
                            input_capture_enable_options,
                            G_N_ELEMENTS (input_capture_enable_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* Lenient: Enable() is a noop for anything but a disabled session */
  session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;

  xdp_dbus_impl_input_capture_call_enable (input_capture->impl,
                                           arg_session_handle,
                                           xdp_app_info_get_id (app_info),
                                           options,
                                           NULL,
                                           NULL,
                                           NULL);

  xdp_dbus_input_capture_complete_enable (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_disable (XdpDbusInputCapture   *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_session_handle,
                GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Not connected to EIS");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = filter_options (arg_options,
                            input_capture_disable_options,
                            G_N_ELEMENTS (input_capture_disable_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* Lenient: a caller may call Disable() before processing a Disabled signal */
  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_assert_not_reached ();
    }

  xdp_dbus_impl_input_capture_call_disable (input_capture->impl,
                                            arg_session_handle,
                                            xdp_app_info_get_id (app_info),
                                            options,
                                            NULL,
                                            NULL,
                                            NULL);

  xdp_dbus_input_capture_complete_disable (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_release (XdpDbusInputCapture   *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_session_handle,
                GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Not connected to EIS");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = filter_options (arg_options,
                            input_capture_release_options,
                            G_N_ELEMENTS (input_capture_release_options),
                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* Lenient: a caller may call Release() before processing a Deactivated signal */
  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_assert_not_reached ();
    }

  xdp_dbus_impl_input_capture_call_release (input_capture->impl,
                                            arg_session_handle,
                                            xdp_app_info_get_id (app_info),
                                            options,
                                            NULL,
                                            NULL,
                                            NULL);

  xdp_dbus_input_capture_complete_release (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_connect_to_eis (XdpDbusInputCapture   *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList           *in_fd_list,
                       const char            *arg_session_handle,
                       GVariant              *arg_options)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpInputCaptureSession *session;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) empty =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) fd = NULL;

  session = lookup_session (input_capture, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Already connected");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_FAILED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_dbus_impl_input_capture_call_connect_to_eis_sync (input_capture->impl,
                                                             arg_session_handle,
                                                             xdp_app_info_get_id (app_info),
                                                             g_variant_builder_end (&empty),
                                                             in_fd_list,
                                                             &fd,
                                                             &out_fd_list,
                                                             NULL,
                                                             &error))
    {
      g_warning ("Failed to ConnectToEIS: %s", error->message);
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;

  xdp_dbus_input_capture_complete_connect_to_eis (object,
                                                  g_steal_pointer (&invocation),
                                                  out_fd_list, fd);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_input_capture_iface_init (XdpDbusInputCaptureIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_create_session2 = handle_create_session2;
  iface->handle_start = handle_start;
  iface->handle_get_zones = handle_get_zones;
  iface->handle_set_pointer_barriers = handle_set_pointer_barriers;
  iface->handle_connect_to_eis = handle_connect_to_eis;
  iface->handle_enable = handle_enable;
  iface->handle_disable = handle_disable;
  iface->handle_release = handle_release;
}

static void
pass_signal (XdpInputCapture        *input_capture,
             XdpInputCaptureSession *session,
             const char             *signal_name,
             const char             *session_id,
             GVariant               *options)
{
  GDBusConnection *connection =
    g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (input_capture));
  XdpAppInfo *app_info = xdp_session_dex_get_app_info (session->session);

#if HAVE_VARLINK
  if (session->is_varlink)
    {
      if (g_strcmp0 (signal_name, "ZonesChanged") == 0)
        varlink_zones_changed (input_capture, session);
      else if (g_strcmp0 (signal_name, "Activated") == 0)
        varlink_capture_status_changed (session, "activated", options);
      else if (g_strcmp0 (signal_name, "Deactivated") == 0)
        varlink_capture_status_changed (session, "deactivated", options);
      else if (g_strcmp0 (signal_name, "Disabled") == 0)
        varlink_capture_status_changed (session, "disabled", options);

      return;
    }
#endif

  if (!connection)
    return;

  g_dbus_connection_emit_signal (connection,
                                 xdp_app_info_get_sender (app_info),
                                 DESKTOP_DBUS_PATH,
                                 INPUT_CAPTURE_DBUS_IFACE,
                                 signal_name,
                                 g_variant_new ("(o@a{sv})", session_id, options),
                                 NULL);
}

static XdpInputCaptureSession *
session_for_signal (XdpInputCapture *input_capture,
                    const char      *session_id)
{
  XdpInputCaptureSession *session;

  session = lookup_session (input_capture, session_id, NULL);
  if (!session)
    g_critical ("Invalid session type for signal");

  return session;
}

static void
on_disabled_cb (XdpDbusImplInputCapture *impl,
                const char              *session_id,
                GVariant                *options,
                gpointer                 data)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (data);
  XdpInputCaptureSession *session;

  session = session_for_signal (input_capture, session_id);
  if (!session)
    return;

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        pass_signal (input_capture, session, "Disabled", session_id, options);
        session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
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
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (data);
  XdpInputCaptureSession *session;

  session = session_for_signal (input_capture, session_id);
  if (!session)
    return;

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
        pass_signal (input_capture, session, "Activated", session_id, options);
        session->state = INPUT_CAPTURE_SESSION_STATE_ACTIVE;
        break;
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
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
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (data);
  XdpInputCaptureSession *session;

  session = session_for_signal (input_capture, session_id);
  if (!session)
    return;

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        pass_signal (input_capture, session, "Deactivated", session_id, options);
        session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        break;
    }
}

static void
on_zones_changed_cb (XdpDbusImplInputCapture *impl,
                     const char              *session_id,
                     GVariant                *options,
                     gpointer                 data)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (data);
  XdpInputCaptureSession *session;

  session = session_for_signal (input_capture, session_id);
  if (!session)
    return;

  switch (session->state)
    {
    case INPUT_CAPTURE_SESSION_STATE_STARTED:
    case INPUT_CAPTURE_SESSION_STATE_ENABLED:
    case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
    case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      pass_signal (input_capture, session, "ZonesChanged", session_id, options);
      break;
    case INPUT_CAPTURE_SESSION_STATE_INIT:
    case INPUT_CAPTURE_SESSION_STATE_CLOSED:
      break;
    }
}


#if HAVE_VARLINK

#define VARLINK_INTERFACE "org.freedesktop.portal.InputCapture"

static const char varlink_interface_description[] =
  "# The InputCapture portal allows capturing input events from connected\n"
  "# physical or logical devices. Once the compositor activates input\n"
  "# capturing, events from physical or logical devices are sent directly to\n"
  "# the application instead of being used to update the pointer position\n"
  "# on-screen.\n"
  "#\n"
  "# Input capture has two distinct states. Enabled: the application has\n"
  "# requested that input should be captured when certain conditions are met,\n"
  "# but no input events are being delivered yet; the application controls\n"
  "# this state via Enable and Disable. Active: input events are being\n"
  "# delivered to the application, and only the compositor decides when to\n"
  "# enter this state.\n"
  "#\n"
  "# The compositor is always in control of input capturing and may filter\n"
  "# events or stop capturing at any time. There is currently no way for an\n"
  "# application to activate immediate input capture.\n"
  "#\n"
  "# Input capturing is trigger-based. Currently the only defined trigger is\n"
  "# pointer barriers: horizontal or vertical lines at the edges of screen\n"
  "# zones that trigger when the cursor moves across them\n"
  "interface " VARLINK_INTERFACE "\n"
  "\n"
  "# The capabilities implemented by the portal. This list is constant, it is\n"
  "# not the list of capabilities currently available but rather which\n"
  "# capabilities are implemented by the portal.\n"
  "#\n"
  "# Applications must ignore unknown capabilities\n"
  "method GetSupportedCapabilities() -> (capabilities: []Capability)\n"
  "\n"
  "# Create an input capture session. A successfully created session may at\n"
  "# any time be closed by the portal implementation, and closing the\n"
  "# connection closes the session.\n"
  "#\n"
  "# The session must be started with Start before using methods in this\n"
  "# interface which take a session.\n"
  "#\n"
  "# To capture clipboard content, you can call Clipboard.RequestClipboard.\n"
  "# When clipboard access has been granted, see clipboard_enabled in Start,\n"
  "# the clipboard may be accessed, but only when the session is active.\n"
  "#\n"
  "# Call with `more` on a dedicated connection. The first reply carries the\n"
  "# session id, and the final reply means the session ended\n"
  "method CreateSession() -> (session: int)\n"
  "\n"
  "# Start the input capture session. This will typically result in the portal\n"
  "# presenting a dialog letting the user decide whether they want to allow\n"
  "# the input of the session to be captured, and what capabilities to\n"
  "# support. This method may only be called once on a session.\n"
  "#\n"
  "# The capabilities available to this session are always a subset of the\n"
  "# requested capabilities. Note that while a capability may be available to\n"
  "# a session, there is no guarantee a device with that capability is\n"
  "# currently available or if one does become available that it will trigger\n"
  "# input capture. It is best to view this set as a negative confirmation - a\n"
  "# capability that was requested but is missing is an indication that this\n"
  "# application may not capture events of that capability.\n"
  "#\n"
  "# If the stored session cannot be restored, restore_token is ignored and\n"
  "# the user will be prompted normally. This may happen when, for example,\n"
  "# the session contains capabilities that are not available anymore, or when\n"
  "# the stored permissions are withdrawn. The restore token is invalidated\n"
  "# after using it once. To restore the same session again, use the new\n"
  "# restore token sent in response to starting this session\n"
  "method Start(\n"
  "  session: int,\n"
  "  parent_window: string,\n"
  "  capabilities: []Capability,\n"
  "  restore_token: ?string,\n"
  "  persist_mode: ?PersistMode\n"
  ") -> (\n"
  "  capabilities: []Capability,\n"
  "  clipboard_enabled: bool,\n"
  "  restore_token: ?string\n"
  ")\n"
  "\n"
  "# Retrieve the set of currently available input zones for this session. The\n"
  "# zones may not be continuous and may be a logical representation of the\n"
  "# physical screens (e.g. a 4k screen may be represented as low-resolution\n"
  "# screen instead). A set of zones is identified by a unique zone_set.\n"
  "#\n"
  "# Note that zones are session-specific, there is no guarantee that two\n"
  "# applications see the same screen zones. An empty zone list implies that\n"
  "# no pointer barriers can be set.\n"
  "#\n"
  "# To establish a pointer barrier, the application must pass zone_set to\n"
  "# SetPointerBarriers\n"
  "method GetZones(session: int) -> (zones: []Zone, zone_set: int)\n"
  "\n"
  "# Set up zero or more pointer barriers. Pointer barriers are horizontal or\n"
  "# vertical lines that should trigger the start of input capture when the\n"
  "# cursor moves across the pointer barrier. After a successful Enable call\n"
  "# and when the compositor has deemed the pointer barrier to be crossed,\n"
  "# input events are sent to the application via the transport layer.\n"
  "#\n"
  "# A pointer barrier must be situated at the outside boundary of the union\n"
  "# of all zones, and must be fully contained within one zone. A pointer\n"
  "# barrier is considered triggered when the pointer would logically move off\n"
  "# that zone, even if the actual cursor movement is clipped to the zone.\n"
  "#\n"
  "# A zero-sized array of pointer barriers removes all existing pointer\n"
  "# barriers for this session. Setting pointer barriers immediately suspends\n"
  "# the current session and the application must call Enable after this\n"
  "# method.\n"
  "#\n"
  "# The zone_set must be equivalent to the last returned zone_set of\n"
  "# GetZones. failed_barriers is an array of barrier_ids of pointer barriers\n"
  "# that have been denied\n"
  "method SetPointerBarriers(\n"
  "  session: int,\n"
  "  barriers: []Barrier,\n"
  "  zone_set: int\n"
  ") -> (failed_barriers: []int)\n"
  "\n"
  "# Enable input capturing. This does not immediately trigger capture, it\n"
  "# merely enables the capturing to be triggered at some future point (e.g.\n"
  "# by the cursor moving across a barrier)\n"
  "method Enable(session: int) -> ()\n"
  "\n"
  "# Disable input capturing. Due to the asynchronous nature of this protocol,\n"
  "# deactivated and disabled statuses may nevertheless be received by the\n"
  "# application after a call to Disable. Input events will not be captured\n"
  "# until a subsequent Enable call\n"
  "method Disable(session: int) -> ()\n"
  "\n"
  "# Release any ongoing input capture. The activation_id specifies which\n"
  "# currently ongoing input capture should be terminated. The asynchronous\n"
  "# nature of this portal allows for an input capture to be deactivated and a\n"
  "# new input capture to be activated before the client requests the Release\n"
  "# for the previous input capture. A compositor should ignore a Release\n"
  "# request for a no longer active activation_id.\n"
  "#\n"
  "# cursor_position is the suggested cursor position within the zones\n"
  "# available in this session. This is a suggestion to the compositor to\n"
  "# place the cursor in the correct position to allow for fluent movement\n"
  "# between virtual screens. The compositor is not required to honor this\n"
  "# suggestion\n"
  "method Release(\n"
  "  session: int,\n"
  "  activation_id: ?int,\n"
  "  cursor_position: ?Position\n"
  ") -> ()\n"
  "\n"
  "# Set up the connection to an active EIS implementation. Once input\n"
  "# capturing starts, input events are sent via the EI protocol between the\n"
  "# compositor and the application. This call must be invoked before Enable.\n"
  "#\n"
  "# A session only needs to set this up once, the EIS implementation is not\n"
  "# affected by calls to Disable and Enable, and the same connection can be\n"
  "# re-used until the session is closed\n"
  "method ConnectToEIS(session: int) -> (fd_idx: int)\n"
  "\n"
  "# Reports capture status transitions. Call with `more` on a dedicated\n"
  "# connection, and the final reply means the session ended.\n"
  "#\n"
  "# activation_id is a number that can be used to synchronize with the\n"
  "# transport-layer. This number has no intrinsic meaning but is guaranteed\n"
  "# to increase by an unspecified amount on each call, and applications must\n"
  "# be able to handle it wrapping around.\n"
  "#\n"
  "# cursor_position is the current cursor position in the same coordinate\n"
  "# space as the zones. Note that this position is usually outside the zones\n"
  "# available to this session as all pointer barriers are at the edge of\n"
  "# their respective zones.\n"
  "#\n"
  "# barrier_id is the barrier that triggered. If it is zero, the pointer\n"
  "# barrier could not be determined. If it is missing, the input capture was\n"
  "# not triggered by a pointer barrier\n"
  "method SubscribeCaptureStatus(session: int) -> (\n"
  "  status: CaptureStatus,\n"
  "  activation_id: ?int,\n"
  "  cursor_position: ?Position,\n"
  "  barrier_id: ?int\n"
  ")\n"
  "\n"
  "# Reports the set of zones available to this session, and again whenever\n"
  "# they change, so GetZones is never needed. A change means the application\n"
  "# must re-establish the pointer barriers. Call with `more` on a dedicated\n"
  "# connection\n"
  "method SubscribeZones(session: int) -> (zones: []Zone, zone_set: int)\n"
  "\n"
  "# A class of devices whose events may be captured\n"
  "type Capability (keyboard, pointer, touchscreen)\n"
  "\n"
  "# How this session should persist: not at all, as long as the application\n"
  "# is running, or until explicitly revoked\n"
  "type PersistMode (none, transient, persistent)\n"
  "\n"
  "# Whether input events are being delivered to the application\n"
  "type CaptureStatus (activated, deactivated, disabled)\n"
  "\n"
  "# A region, specifying that zone's width, height and x/y offset. The name\n"
  "# Zone was chosen to provide distinction with the libei Region\n"
  "type Zone (width: int, height: int, x: int, y: int)\n"
  "\n"
  "# A position in the session's coordinate space\n"
  "type Position (x: float, y: float)\n"
  "\n"
  "# barrier_id is the non-zero ID of this barrier, reported back as the\n"
  "# barrier that triggered input capture. A horizontal pointer barrier must\n"
  "# have y1 == y2, a vertical pointer barrier must have x1 == x2. Diagonal\n"
  "# pointer barriers are not supported\n"
  "type Barrier (barrier_id: int, x1: int, y1: int, x2: int, y2: int)\n"
  "\n"
  "# No session with this id exists, or the caller is not authorized for it\n"
  "error NoSuchSession()\n"
  "\n"
  "# The session is not in a state where this call is allowed\n"
  "error NotAllowed()\n"
  "\n"
  "# A mismatch of ids implies the application is not using the current zone\n"
  "# set and pointer barriers will fail\n"
  "error InvalidZoneSet()\n"
  "\n"
  "# No capability was requested, or a requested one is not supported\n"
  "error InvalidCapability()\n"
  "\n"
  "# The user dismissed the request\n"
  "error Cancelled()\n"
  "\n"
  "# The call must be made with `more`\n"
  "error ExpectedMore()\n"
  "\n"
  "error " XDP_VARLINK_ERROR_NOT_REGISTERED "()\n";

static const char * const capability_names[] = {
  "keyboard", "pointer", "touchscreen",
};

static const char * const persist_mode_names[] = {
  "none", "transient", "persistent",
};

/* Capabilities are a bitmask on D-Bus and a list of names here */
static VarlinkArray *
capabilities_to_varlink (uint32_t capabilities)
{
  VarlinkArray *array;

  varlink_array_new (&array);

  for (size_t i = 0; i < G_N_ELEMENTS (capability_names); i++)
    {
      if (capabilities & (1 << i))
        varlink_array_append_string (array, capability_names[i]);
    }

  return array;
}

static gboolean
capabilities_from_varlink (VarlinkArray *array,
                           uint32_t     *capabilities_out)
{
  unsigned long n_elements = varlink_array_get_n_elements (array);
  uint32_t capabilities = 0;

  for (unsigned long i = 0; i < n_elements; i++)
    {
      const char *name;
      gboolean found = FALSE;

      if (varlink_array_get_string (array, i, &name) < 0)
        return FALSE;

      for (size_t bit = 0; bit < G_N_ELEMENTS (capability_names); bit++)
        {
          if (g_strcmp0 (name, capability_names[bit]) == 0)
            {
              capabilities |= 1 << bit;
              found = TRUE;
              break;
            }
        }

      if (!found)
        return FALSE;
    }

  *capabilities_out = capabilities;

  return TRUE;
}

static VarlinkArray *
zones_to_varlink (GVariant *zones)
{
  GVariantIter iter;
  VarlinkArray *array;
  uint32_t width, height;
  int32_t x, y;

  varlink_array_new (&array);

  if (zones == NULL)
    return array;

  g_variant_iter_init (&iter, zones);
  while (g_variant_iter_next (&iter, "(uuii)", &width, &height, &x, &y))
    {
      g_autoptr(VarlinkObject) zone = NULL;

      varlink_object_new (&zone);
      varlink_object_set_int (zone, "width", width);
      varlink_object_set_int (zone, "height", height);
      varlink_object_set_int (zone, "x", x);
      varlink_object_set_int (zone, "y", y);

      varlink_array_append_object (array, zone);
    }

  return array;
}

/* Barriers cross as a vardict per barrier, matching the D-Bus argument */
static GVariant *
barriers_from_varlink (VarlinkArray *array,
                       gboolean     *valid)
{
  unsigned long n_elements = varlink_array_get_n_elements (array);
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aa{sv}"));

  *valid = FALSE;

  for (unsigned long i = 0; i < n_elements; i++)
    {
      g_auto(GVariantBuilder) barrier_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      VarlinkObject *barrier;
      int64_t barrier_id, x1, y1, x2, y2;

      if (varlink_array_get_object (array, i, &barrier) < 0 ||
          varlink_object_get_int (barrier, "barrier_id", &barrier_id) < 0 ||
          varlink_object_get_int (barrier, "x1", &x1) < 0 ||
          varlink_object_get_int (barrier, "y1", &y1) < 0 ||
          varlink_object_get_int (barrier, "x2", &x2) < 0 ||
          varlink_object_get_int (barrier, "y2", &y2) < 0)
        return NULL;

      g_variant_builder_add (&barrier_builder, "{sv}",
                             "barrier_id", g_variant_new_uint32 (barrier_id));
      g_variant_builder_add (&barrier_builder, "{sv}",
                             "position",
                             g_variant_new ("(iiii)", x1, y1, x2, y2));

      g_variant_builder_add (&builder, "@a{sv}",
                             g_variant_builder_end (&barrier_builder));
    }

  *valid = TRUE;

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* Documented as a pair of doubles, but a backend with only whole pixels to
 * report may send integers */
static VarlinkObject *
position_to_varlink (GVariant *position)
{
  VarlinkObject *object;
  double x, y;

  if (g_variant_is_of_type (position, G_VARIANT_TYPE ("(dd)")))
    {
      g_variant_get (position, "(dd)", &x, &y);
    }
  else if (g_variant_is_of_type (position, G_VARIANT_TYPE ("(ii)")))
    {
      int32_t int_x, int_y;

      g_variant_get (position, "(ii)", &int_x, &int_y);
      x = int_x;
      y = int_y;
    }
  else
    {
      return NULL;
    }

  varlink_object_new (&object);
  varlink_object_set_float (object, "x", x);
  varlink_object_set_float (object, "y", y);

  return object;
}

static XdpInputCaptureSession *
varlink_lookup_session (XdpInputCapture      *input_capture,
                        XdpVarlinkConnection *connection,
                        VarlinkObject        *parameters,
                        long                 *error_out)
{
  XdpInputCaptureSession *session;
  g_autofree char *key = NULL;
  int64_t handle;

  if (varlink_object_get_int (parameters, "session", &handle) < 0)
    {
      *error_out = -1;
      return NULL;
    }

  key = g_strdup_printf ("%" G_GINT64_FORMAT, handle);
  session = lookup_session (input_capture, key,
                            xdp_varlink_connection_get_app_info (connection));
  if (session == NULL)
    *error_out = 0;

  return session;
}

/* Replies to every parked call in @calls and empties it */
static void
flush_subscriptions (GPtrArray *calls,
                     uint64_t   reply_flags)
{
  for (guint i = 0; i < calls->len; i++)
    {
      g_autoptr(VarlinkObject) reply = NULL;

      varlink_object_new (&reply);
      xdp_varlink_call_reply (g_ptr_array_index (calls, i), reply, reply_flags);
    }

  g_ptr_array_set_size (calls, 0);
}

static void
varlink_session_closed (XdpInputCaptureSession *session)
{
  flush_subscriptions (session->capture_status_calls, 0);
  flush_subscriptions (session->zones_calls, 0);
}

static void
varlink_capture_status_changed (XdpInputCaptureSession *session,
                                const char             *status,
                                GVariant               *options)
{
  GPtrArray *calls = session->capture_status_calls;
  uint32_t activation_id;
  uint32_t barrier_id;
  g_autoptr(GVariant) cursor_position = NULL;
  gboolean have_activation_id;
  gboolean have_barrier_id;

  have_activation_id =
    g_variant_lookup (options, "activation_id", "u", &activation_id);
  have_barrier_id =
    g_variant_lookup (options, "barrier_id", "u", &barrier_id);
  cursor_position = g_variant_lookup_value (options, "cursor_position", NULL);

  for (guint i = calls->len; i > 0; i--)
    {
      g_autoptr(VarlinkObject) reply = NULL;

      varlink_object_new (&reply);
      varlink_object_set_string (reply, "status", status);

      if (have_activation_id)
        varlink_object_set_int (reply, "activation_id", activation_id);

      if (have_barrier_id)
        varlink_object_set_int (reply, "barrier_id", barrier_id);

      if (cursor_position)
        {
          g_autoptr(VarlinkObject) position = position_to_varlink (cursor_position);

          if (position)
            varlink_object_set_object (reply, "cursor_position", position);
        }

      if (xdp_varlink_call_reply (g_ptr_array_index (calls, i - 1), reply,
                                  VARLINK_REPLY_CONTINUES) ==
          -VARLINK_ERROR_CONNECTION_CLOSED)
        g_ptr_array_remove_index_fast (calls, i - 1);
    }
}

/* Sends the current zones to @call, or to every subscription when @call is
 * NULL; the D-Bus ZonesChanged signal only invalidates the set */
static void
send_zones (XdpInputCapture        *input_capture,
            XdpInputCaptureSession *session,
            VarlinkCall            *call)
{
  XdpAppInfo *app_info = xdp_session_dex_get_app_info (session->session);
  g_autoptr(XdpDbusImplInputCaptureGetZonesResult) result = NULL;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(VarlinkArray) zones = NULL;
  g_autoptr(GVariant) zones_variant = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_auto(GVariantBuilder) request_options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) request_options = NULL;
  uint32_t zone_set = 0;

  request_options = g_variant_ref_sink (g_variant_builder_end (&request_options_builder));

  request = dex_await_object (
    xdp_request_dex_new (input_capture->context,
                         app_info,
                         G_DBUS_INTERFACE_SKELETON (input_capture),
                         G_DBUS_PROXY (input_capture->impl),
                         request_options),
    &error);
  if (!request)
    return;

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_get_zones_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      input_capture_session_get_object_path (session),
      xdp_app_info_get_id (app_info),
      g_variant_builder_end (&options_builder)),
    &error);

  if (!result || result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    return;

  zones_variant = g_variant_lookup_value (result->results, "zones",
                                          G_VARIANT_TYPE ("a(uuii)"));
  g_variant_lookup (result->results, "zone_set", "u", &zone_set);

  zones = zones_to_varlink (zones_variant);

  if (call)
    {
      g_autoptr(VarlinkObject) reply = NULL;

      varlink_object_new (&reply);
      varlink_object_set_array (reply, "zones", zones);
      varlink_object_set_int (reply, "zone_set", zone_set);

      xdp_varlink_call_reply (call, reply, VARLINK_REPLY_CONTINUES);
      return;
    }

  for (guint i = session->zones_calls->len; i > 0; i--)
    {
      g_autoptr(VarlinkObject) reply = NULL;

      varlink_object_new (&reply);
      varlink_object_set_array (reply, "zones", zones);
      varlink_object_set_int (reply, "zone_set", zone_set);

      if (xdp_varlink_call_reply (g_ptr_array_index (session->zones_calls, i - 1),
                                  reply, VARLINK_REPLY_CONTINUES) ==
          -VARLINK_ERROR_CONNECTION_CLOSED)
        g_ptr_array_remove_index_fast (session->zones_calls, i - 1);
    }
}

static DexFuture *
zones_changed_fiber (XdpInputCapture        *input_capture,
                     XdpInputCaptureSession *session)
{
  if (session->zones_calls->len > 0)
    send_zones (input_capture, session, NULL);

  return dex_future_new_true ();
}

static void
varlink_zones_changed (XdpInputCapture        *input_capture,
                       XdpInputCaptureSession *session)
{
  dex_future_disown (dex_scheduler_spawnv (NULL, 0,
                                           G_CALLBACK (zones_changed_fiber),
                                           2,
                                           XDP_TYPE_INPUT_CAPTURE, input_capture,
                                           XDP_TYPE_INPUT_CAPTURE_SESSION, session));
}

static long
reply_no_such_session (VarlinkCall *call)
{
  return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSuchSession", NULL);
}

static long
reply_not_allowed (VarlinkCall *call)
{
  return varlink_call_reply_error (call, VARLINK_INTERFACE ".NotAllowed", NULL);
}

/* The impl side travels over D-Bus, so it needs a request object even though
 * no varlink client sees one */
static XdpRequestDex *
varlink_request_new (XdpInputCapture      *input_capture,
                     XdpVarlinkConnection *connection,
                     GError              **error)
{
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  return dex_await_object (
    xdp_request_dex_new (input_capture->context,
                         xdp_varlink_connection_get_app_info (connection),
                         G_DBUS_INTERFACE_SKELETON (input_capture),
                         G_DBUS_PROXY (input_capture->impl),
                         options),
    error);
}

static long
handle_varlink_get_supported_capabilities (XdpVarlinkService    *service,
                                           XdpVarlinkConnection *connection,
                                           VarlinkCall          *call,
                                           VarlinkObject        *parameters,
                                           uint64_t              flags,
                                           gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(VarlinkArray) capabilities = NULL;

  capabilities = capabilities_to_varlink (
    xdp_dbus_input_capture_get_supported_capabilities (
      XDP_DBUS_INPUT_CAPTURE (input_capture)));

  varlink_object_new (&reply);
  varlink_object_set_array (reply, "capabilities", capabilities);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_create_session (XdpVarlinkService    *service,
                               XdpVarlinkConnection *connection,
                               VarlinkCall          *call,
                               VarlinkObject        *parameters,
                               uint64_t              flags,
                               gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  g_autoptr(XdpInputCaptureSession) session = NULL;
  g_autoptr(XdpSessionDex) session_dex = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(GError) error = NULL;
  int64_t handle;

  if (!(flags & VARLINK_CALL_MORE))
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".ExpectedMore", NULL);

  if (input_capture->impl_version < 2)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".NotAllowed", NULL);

  handle = xdp_varlink_session_next_handle ();

  session_dex = dex_await_object (
    xdp_session_dex_new_for_varlink (input_capture->context,
                                     app_info,
                                     G_DBUS_PROXY (input_capture->impl),
                                     handle),
    &error);
  if (!session_dex)
    {
      g_warning ("Failed to create session: %s", error->message);
      return varlink_call_reply_error (call, VARLINK_INTERFACE ".NotAllowed", NULL);
    }

  session = xdp_input_capture_session_new (input_capture,
                                           g_steal_pointer (&session_dex));
  session->is_varlink = TRUE;

  {
    g_auto(GVariantBuilder) empty_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
    g_autoptr(XdpDbusImplInputCaptureCreateSession2Result) create_result = NULL;

    create_result = dex_await_boxed (
      xdp_dbus_impl_input_capture_call_create_session2_future (
        input_capture->impl,
        input_capture_session_get_object_path (session),
        xdp_app_info_get_id (app_info),
        g_variant_builder_end (&empty_builder)),
      &error);

    if (!create_result)
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("A backend call failed: %s", error->message);
        xdp_session_dex_close (session->session, FALSE);
        return varlink_call_reply_error (call, VARLINK_INTERFACE ".NotAllowed", NULL);
      }
  }

  xdp_varlink_session_park (session->session, connection, call);

  xdp_session_dex_store_take_session (input_capture->sessions,
                                      g_steal_pointer (&session));

  varlink_object_new (&reply);
  varlink_object_set_int (reply, "session", handle);

  return varlink_call_reply (call, reply, VARLINK_REPLY_CONTINUES);
}

static long
handle_varlink_start (XdpVarlinkService    *service,
                      XdpVarlinkConnection *connection,
                      VarlinkCall          *call,
                      VarlinkObject        *parameters,
                      uint64_t              flags,
                      gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplInputCaptureStartResult) start_result = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(VarlinkArray) granted = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  VarlinkArray *capabilities_array;
  const char *parent_window = NULL;
  const char *restore_token = NULL;
  const char *persist_mode = NULL;
  uint32_t capabilities = 0;
  uint32_t granted_capabilities = 0;
  gboolean clipboard_enabled = FALSE;
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  if (session->state != INPUT_CAPTURE_SESSION_STATE_INIT)
    return reply_not_allowed (call);

  if (varlink_object_get_array (parameters, "capabilities", &capabilities_array) < 0 ||
      !capabilities_from_varlink (capabilities_array, &capabilities) ||
      capabilities == 0)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".InvalidCapability", NULL);

  if (varlink_object_get_string (parameters, "parent_window", &parent_window) < 0)
    parent_window = "";

  g_variant_builder_add (&options_builder, "{sv}",
                         "capabilities", g_variant_new_uint32 (capabilities));

  if (varlink_object_get_string (parameters, "restore_token", &restore_token) >= 0 &&
      restore_token != NULL)
    {
      if (!xdp_session_persistence_validate_restore_token (restore_token, &error))
        return varlink_call_reply_invalid_parameter (call, "restore_token");

      g_variant_builder_add (&options_builder, "{sv}",
                             "restore_token", g_variant_new_string (restore_token));
    }

  if (varlink_object_get_string (parameters, "persist_mode", &persist_mode) >= 0 &&
      persist_mode != NULL)
    {
      size_t mode;

      for (mode = 0; mode < G_N_ELEMENTS (persist_mode_names); mode++)
        {
          if (g_strcmp0 (persist_mode, persist_mode_names[mode]) == 0)
            break;
        }

      if (mode == G_N_ELEMENTS (persist_mode_names))
        return varlink_call_reply_invalid_parameter (call, "persist_mode");

      g_variant_builder_add (&options_builder, "{sv}",
                             "persist_mode", g_variant_new_uint32 (mode));
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  request = varlink_request_new (input_capture, connection, &error);
  if (!request)
    return reply_not_allowed (call);

  replace_restore_token_with_data (session, app_info, &options);

  start_result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_start_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      input_capture_session_get_object_path (session),
      xdp_app_info_get_id (app_info),
      parent_window,
      options),
    &error);

  if (!start_result)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      return reply_not_allowed (call);
    }

  if (start_result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".Cancelled", NULL);

  {
    g_auto(GVariantBuilder) results_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
    g_autoptr(GVariant) results = NULL;

    if (!collect_start_results (session, app_info, start_result->results,
                                &results_builder))
      return reply_not_allowed (call);

    results = g_variant_ref_sink (g_variant_builder_end (&results_builder));

    g_variant_lookup (results, "capabilities", "u", &granted_capabilities);
    g_variant_lookup (results, "clipboard_enabled", "b", &clipboard_enabled);
  }

  granted = capabilities_to_varlink (granted_capabilities);

  varlink_object_new (&reply);
  varlink_object_set_array (reply, "capabilities", granted);
  varlink_object_set_bool (reply, "clipboard_enabled", clipboard_enabled);
  if (session->restore_token)
    varlink_object_set_string (reply, "restore_token", session->restore_token);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_get_zones (XdpVarlinkService    *service,
                          XdpVarlinkConnection *connection,
                          VarlinkCall          *call,
                          VarlinkObject        *parameters,
                          uint64_t              flags,
                          gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplInputCaptureGetZonesResult) result = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(VarlinkArray) zones = NULL;
  g_autoptr(GVariant) zones_variant = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  uint32_t zone_set = 0;
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        return reply_not_allowed (call);
    }

  request = varlink_request_new (input_capture, connection, &error);
  if (!request)
    return reply_not_allowed (call);

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_get_zones_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      input_capture_session_get_object_path (session),
      xdp_app_info_get_id (app_info),
      g_variant_builder_end (&options_builder)),
    &error);

  if (!result || result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    return reply_not_allowed (call);

  zones_variant = g_variant_lookup_value (result->results, "zones",
                                          G_VARIANT_TYPE ("a(uuii)"));
  g_variant_lookup (result->results, "zone_set", "u", &zone_set);

  zones = zones_to_varlink (zones_variant);

  varlink_object_new (&reply);
  varlink_object_set_array (reply, "zones", zones);
  varlink_object_set_int (reply, "zone_set", zone_set);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_set_pointer_barriers (XdpVarlinkService    *service,
                                     XdpVarlinkConnection *connection,
                                     VarlinkCall          *call,
                                     VarlinkObject        *parameters,
                                     uint64_t              flags,
                                     gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(XdpDbusImplInputCaptureSetPointerBarriersResult) result = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(VarlinkArray) failed = NULL;
  g_autoptr(GVariant) barriers = NULL;
  g_autoptr(GVariant) failed_variant = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  VarlinkArray *barriers_array;
  gboolean valid;
  int64_t zone_set;
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        return reply_not_allowed (call);
    }

  if (varlink_object_get_array (parameters, "barriers", &barriers_array) < 0)
    return varlink_call_reply_invalid_parameter (call, "barriers");

  if (varlink_object_get_int (parameters, "zone_set", &zone_set) < 0)
    return varlink_call_reply_invalid_parameter (call, "zone_set");

  barriers = barriers_from_varlink (barriers_array, &valid);
  if (!valid)
    return varlink_call_reply_invalid_parameter (call, "barriers");

  request = varlink_request_new (input_capture, connection, &error);
  if (!request)
    return reply_not_allowed (call);

  result = dex_await_boxed (
    xdp_dbus_impl_input_capture_call_set_pointer_barriers_future (
      input_capture->impl,
      xdp_request_dex_get_object_path (request),
      input_capture_session_get_object_path (session),
      xdp_app_info_get_id (app_info),
      g_variant_builder_end (&options_builder),
      barriers,
      zone_set),
    &error);

  if (!result)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".InvalidZoneSet", NULL);

  if (result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    return reply_not_allowed (call);

  varlink_array_new (&failed);

  failed_variant = g_variant_lookup_value (result->results, "failed_barriers",
                                           G_VARIANT_TYPE ("au"));
  if (failed_variant)
    {
      GVariantIter iter;
      uint32_t barrier_id;

      g_variant_iter_init (&iter, failed_variant);
      while (g_variant_iter_next (&iter, "u", &barrier_id))
        varlink_array_append_int (failed, barrier_id);
    }

  varlink_object_new (&reply);
  varlink_object_set_array (reply, "failed_barriers", failed);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_enable (XdpVarlinkService    *service,
                       XdpVarlinkConnection *connection,
                       VarlinkCall          *call,
                       VarlinkObject        *parameters,
                       uint64_t              flags,
                       gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(VarlinkObject) reply = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        return reply_not_allowed (call);
    }

  session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;

  xdp_dbus_impl_input_capture_call_enable (input_capture->impl,
                                           input_capture_session_get_object_path (session),
                                           xdp_app_info_get_id (app_info),
                                           g_variant_builder_end (&options_builder),
                                           NULL, NULL, NULL);

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_disable (XdpVarlinkService    *service,
                        XdpVarlinkConnection *connection,
                        VarlinkCall          *call,
                        VarlinkObject        *parameters,
                        uint64_t              flags,
                        gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(VarlinkObject) reply = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        return reply_not_allowed (call);
    }

  xdp_dbus_impl_input_capture_call_disable (input_capture->impl,
                                            input_capture_session_get_object_path (session),
                                            xdp_app_info_get_id (app_info),
                                            g_variant_builder_end (&options_builder),
                                            NULL, NULL, NULL);

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_release (XdpVarlinkService    *service,
                        XdpVarlinkConnection *connection,
                        VarlinkCall          *call,
                        VarlinkObject        *parameters,
                        uint64_t              flags,
                        gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(VarlinkObject) reply = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  VarlinkObject *cursor_position;
  int64_t activation_id;
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
        session->state = INPUT_CAPTURE_SESSION_STATE_ENABLED;
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        return reply_not_allowed (call);
    }

  if (varlink_object_get_int (parameters, "activation_id", &activation_id) >= 0)
    {
      g_variant_builder_add (&options_builder, "{sv}",
                             "activation_id", g_variant_new_uint32 (activation_id));
    }

  if (varlink_object_get_object (parameters, "cursor_position", &cursor_position) >= 0)
    {
      double x = 0, y = 0;

      varlink_object_get_float (cursor_position, "x", &x);
      varlink_object_get_float (cursor_position, "y", &y);

      g_variant_builder_add (&options_builder, "{sv}",
                             "cursor_position", g_variant_new ("(dd)", x, y));
    }

  xdp_dbus_impl_input_capture_call_release (input_capture->impl,
                                            input_capture_session_get_object_path (session),
                                            xdp_app_info_get_id (app_info),
                                            g_variant_builder_end (&options_builder),
                                            NULL, NULL, NULL);

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_connect_to_eis (XdpVarlinkService    *service,
                               XdpVarlinkConnection *connection,
                               VarlinkCall          *call,
                               VarlinkObject        *parameters,
                               uint64_t              flags,
                               gpointer              user_data)
{
  XdpInputCapture *input_capture = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  XdpInputCaptureSession *session;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autofd int fd = -1;
  long lookup_error = 0;

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  switch (session->state)
    {
      case INPUT_CAPTURE_SESSION_STATE_STARTED:
        break;
      case INPUT_CAPTURE_SESSION_STATE_ENABLED:
      case INPUT_CAPTURE_SESSION_STATE_ACTIVE:
      case INPUT_CAPTURE_SESSION_STATE_DISABLED:
      case INPUT_CAPTURE_SESSION_STATE_INIT:
      case INPUT_CAPTURE_SESSION_STATE_CLOSED:
        return reply_not_allowed (call);
    }

  if (!xdp_dbus_impl_input_capture_call_connect_to_eis_sync (
        input_capture->impl,
        input_capture_session_get_object_path (session),
        xdp_app_info_get_id (app_info),
        g_variant_builder_end (&options_builder),
        NULL,
        &fd_handle,
        &out_fd_list,
        NULL,
        &error))
    {
      g_warning ("Failed to ConnectToEIS: %s", error->message);
      return reply_not_allowed (call);
    }

  fd = g_unix_fd_list_get (out_fd_list, g_variant_get_handle (fd_handle), &error);
  if (fd < 0)
    {
      g_warning ("Failed to take the EIS fd: %s", error->message);
      return reply_not_allowed (call);
    }

  if (varlink_call_push_fd (call, fd) < 0)
    return reply_not_allowed (call);

  g_steal_fd (&fd);
  session->state = INPUT_CAPTURE_SESSION_STATE_DISABLED;

  varlink_object_new (&reply);
  varlink_object_set_int (reply, "fd_idx", 0);

  return varlink_call_reply (call, reply, 0);
}

static long
subscribe (XdpInputCapture      *input_capture,
           XdpVarlinkConnection *connection,
           VarlinkCall          *call,
           VarlinkObject        *parameters,
           uint64_t              flags,
           gboolean              zones)
{
  XdpInputCaptureSession *session;
  long lookup_error = 0;

  if (!(flags & VARLINK_CALL_MORE))
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".ExpectedMore", NULL);

  session = varlink_lookup_session (input_capture, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  g_ptr_array_add (zones ? session->zones_calls : session->capture_status_calls,
                   varlink_call_ref (call));

  if (zones)
    send_zones (input_capture, session, call);

  return 0;
}

static long
handle_varlink_subscribe_capture_status (XdpVarlinkService    *service,
                                         XdpVarlinkConnection *connection,
                                         VarlinkCall          *call,
                                         VarlinkObject        *parameters,
                                         uint64_t              flags,
                                         gpointer              user_data)
{
  return subscribe (user_data, connection, call, parameters, flags, FALSE);
}

static long
handle_varlink_subscribe_zones (XdpVarlinkService    *service,
                                XdpVarlinkConnection *connection,
                                VarlinkCall          *call,
                                VarlinkObject        *parameters,
                                uint64_t              flags,
                                gpointer              user_data)
{
  return subscribe (user_data, connection, call, parameters, flags, TRUE);
}

static const XdpVarlinkMethod varlink_methods[] = {
  { "GetSupportedCapabilities", handle_varlink_get_supported_capabilities, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "CreateSession", handle_varlink_create_session, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "Start", handle_varlink_start, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "GetZones", handle_varlink_get_zones, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SetPointerBarriers", handle_varlink_set_pointer_barriers, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "Enable", handle_varlink_enable, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "Disable", handle_varlink_disable, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "Release", handle_varlink_release, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "ConnectToEIS", handle_varlink_connect_to_eis, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SubscribeCaptureStatus", handle_varlink_subscribe_capture_status, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SubscribeZones", handle_varlink_subscribe_zones, XDP_VARLINK_METHOD_FLAGS_NONE },
};

gboolean
init_input_capture_varlink (XdpVarlinkService  *service,
                            XdpContext         *context,
                            GError            **error)
{
  GDBusInterfaceSkeleton *skeleton;

  skeleton = xdp_context_get_portal (context, INPUT_CAPTURE_DBUS_IFACE);
  if (skeleton == NULL)
    return TRUE;

  return xdp_varlink_service_add_interface (service, varlink_interface_description,
                                            varlink_methods,
                                            G_N_ELEMENTS (varlink_methods),
                                            g_object_ref (skeleton),
                                            g_object_unref, error);
}

#endif /* HAVE_VARLINK */

static void
xdp_input_capture_dispose (GObject *object)
{
  XdpInputCapture *input_capture = XDP_INPUT_CAPTURE (object);

  g_clear_object (&input_capture->impl);
  g_clear_object (&input_capture->sessions);

  G_OBJECT_CLASS (xdp_input_capture_parent_class)->dispose (object);
}

static void
xdp_input_capture_init (XdpInputCapture *input_capture)
{
}

static void
xdp_input_capture_class_init (XdpInputCaptureClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = xdp_input_capture_dispose;
}

static XdpInputCapture *
xdp_input_capture_new (XdpContext              *context,
                       XdpDbusImplInputCapture *impl)
{
  XdpInputCapture *input_capture;

  input_capture = g_object_new (XDP_TYPE_INPUT_CAPTURE, NULL);
  input_capture->context = context;
  input_capture->impl = g_object_ref (impl);
  input_capture->sessions =
    xdp_session_dex_store_new_wrapped (XdpInputCaptureSession, session);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (input_capture->impl), G_MAXINT);

  input_capture->impl_version =
    MAX (xdp_dbus_impl_input_capture_get_version (impl), 1);
  xdp_dbus_input_capture_set_version (XDP_DBUS_INPUT_CAPTURE (input_capture),
                                      MIN (input_capture->impl_version, 2));

  g_object_bind_property (G_OBJECT (input_capture->impl), "supported-capabilities",
                          G_OBJECT (input_capture), "supported-capabilities",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (input_capture->impl, "disabled",
                           G_CALLBACK (on_disabled_cb),
                           input_capture,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (input_capture->impl, "activated",
                           G_CALLBACK (on_activated_cb),
                           input_capture,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (input_capture->impl, "deactivated",
                           G_CALLBACK (on_deactivated_cb),
                           input_capture,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (input_capture->impl, "zones-changed",
                           G_CALLBACK (on_zones_changed_cb),
                           input_capture,
                           G_CONNECT_DEFAULT);

  return input_capture;
}

void
init_input_capture (XdpContext *context)
{
  g_autoptr(XdpInputCapture) input_capture = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplInputCapture) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, INPUT_CAPTURE_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_input_capture_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     impl_config->dbus_name,
                                                     DESKTOP_DBUS_PATH,
                                                     NULL,
                                                     &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create input capture proxy: %s", error->message);
      return;
    }

  input_capture = xdp_input_capture_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&input_capture)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
}
