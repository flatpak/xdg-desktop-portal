/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "screen-cast.h"

#include <stdint.h>

#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>

#include "pipewire.h"
#include "remote-desktop.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-pw-keys.h"
#include "xdp-request.h"
#include "xdp-screen-cast.h"
#include "xdp-session-persistence.h"
#include "xdp-session.h"
#include "xdp-utils.h"

#define PERMISSION_ITEM(item_id, item_permissions) \
  ((struct pw_permission) { \
    .id = item_id, \
    .permissions = item_permissions \
  })

typedef struct _ScreenCast ScreenCast;
typedef struct _ScreenCastClass ScreenCastClass;

struct _ScreenCast
{
  XdpDbusScreenCastSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplScreenCast *impl;
};

struct _ScreenCastClass
{
  XdpDbusScreenCastSkeletonClass parent_class;
};

GType screen_cast_get_type (void);
static void screen_cast_iface_init (XdpDbusScreenCastIface *iface);

static GQuark quark_request_session;

struct _ScreenCastStream
{
  uint32_t id;
  int32_t width;
  int32_t height;
  uint64_t pipewire_serial;
  gboolean has_pipewire_serial;
};

G_DEFINE_TYPE_WITH_CODE (ScreenCast, screen_cast,
                         XDP_DBUS_TYPE_SCREEN_CAST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SCREEN_CAST,
                                                screen_cast_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ScreenCast, g_object_unref)

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

  uint32_t audio_types;
  uint32_t exclude_self;
} ScreenCastSession;

typedef struct _ScreenCastSessionClass
{
  XdpSessionClass parent_class;
} ScreenCastSessionClass;

GType screen_cast_session_get_type (void);

G_DEFINE_TYPE (ScreenCastSession, screen_cast_session, xdp_session_get_type ())

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ScreenCastSession, g_object_unref);

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

static void
screen_cast_session_close (XdpSession *session)
{
  ScreenCastSession *screen_cast_session = SCREEN_CAST_SESSION (session);

  screen_cast_session->state = SCREEN_CAST_SESSION_STATE_CLOSED;

  screen_cast_close_audio_session (session);

  xdp_session_persistence_generate_and_save_restore_token (session,
                                                           SCREEN_CAST_PERMISSION_TABLE,
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

static ScreenCastSession *
screen_cast_session_new (ScreenCast  *screen_cast,
                         GVariant    *options,
                         XdpRequest  *request,
                         GError     **error)
{
  XdpSession *session;
  GDBusInterfaceSkeleton *interface_skeleton =
    G_DBUS_INTERFACE_SKELETON (request);
  const char *session_token;
  GDBusConnection *connection =
    g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (screen_cast->impl));
  const char *impl_dbus_name =
    g_dbus_proxy_get_name (G_DBUS_PROXY (screen_cast->impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (screen_cast_session_get_type (), NULL, error,
                            "context", screen_cast->context,
                            "app-info", request->app_info,
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
  XdpDbusImplScreenCast *impl = (XdpDbusImplScreenCast *) source_object;
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
                         "session_handle", g_variant_new_string (xdp_session_get_object_path (session)));

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
handle_create_session (XdpDbusScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  ScreenCast *screen_cast = (ScreenCast *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (screen_cast->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (screen_cast->impl)),
    xdp_request_get_object_path (request),
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = XDP_SESSION (screen_cast_session_new (screen_cast,
                                                  arg_options,
                                                  request,
                                                  &error));
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

  xdp_dbus_impl_screen_cast_call_create_session (screen_cast->impl,
                                                 xdp_request_get_object_path (request),
                                                 xdp_session_get_object_path (session),
                                                 xdp_app_info_get_id (request->app_info),
                                                 options,
                                                 NULL,
                                                 create_session_done,
                                                 g_object_ref (request));

  xdp_dbus_screen_cast_complete_create_session (object, invocation, xdp_request_get_object_path (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
select_sources_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  XdpDbusImplScreenCast *impl = (XdpDbusImplScreenCast *) source_object;
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
validate_source_types (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       gpointer     user_data,
                       GError     **error)
{
  guint32 types = g_variant_get_uint32 (value);
  guint32 supported_types = XDP_SCREEN_CAST_SOURCE_TYPE_MONITOR |
                            XDP_SCREEN_CAST_SOURCE_TYPE_WINDOW |
                            XDP_SCREEN_CAST_SOURCE_TYPE_VIRTUAL;

  if ((types & ~supported_types) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unsupported device type: %x", types & ~supported_types);
      return FALSE;
    } 

  return TRUE;
}

static gboolean
validate_cursor_mode (const char  *key,
                      GVariant    *value,
                      GVariant    *options,
                      gpointer     user_data,
                      GError     **error)
{
  ScreenCast *screen_cast = user_data;
  uint32_t available_cursor_modes =
    xdp_dbus_screen_cast_get_available_cursor_modes (XDP_DBUS_SCREEN_CAST (screen_cast));
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

static const struct {
  XdpScreenCastSourceType source_type;
  XdpScreenCastAudioType audio_types;
} accepted_audio_types[] = {
  { XDP_SCREEN_CAST_SOURCE_TYPE_MONITOR,
    XDP_SCREEN_CAST_AUDIO_TYPE_NONE | XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM },
  { XDP_SCREEN_CAST_SOURCE_TYPE_WINDOW,
    XDP_SCREEN_CAST_AUDIO_TYPE_NONE | XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM |
    XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION },
  { XDP_SCREEN_CAST_SOURCE_TYPE_VIRTUAL,
    XDP_SCREEN_CAST_AUDIO_TYPE_NONE | XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM },
};

static uint32_t
accepted_audio_types_for (uint32_t source_type)
{
  for (size_t i = 0; i < G_N_ELEMENTS (accepted_audio_types); i++)
    {
      if (accepted_audio_types[i].source_type == source_type)
        return accepted_audio_types[i].audio_types;
    }
  return 0;
}

static gboolean
validate_audio_types (const char  *key,
                      GVariant    *value,
                      GVariant    *options,
                      gpointer     user_data,
                      GError     **error)
{
  ScreenCast *screen_cast = user_data;
  uint32_t available_audio_types =
    xdp_dbus_screen_cast_get_available_audio_types (XDP_DBUS_SCREEN_CAST (screen_cast));
  uint32_t audio_types = g_variant_get_uint32 (value);
  uint32_t types = XDP_SCREEN_CAST_SOURCE_TYPE_MONITOR;

  if (audio_types == 0 || (audio_types & ~available_audio_types) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unavailable audio types %x", audio_types);
      return FALSE;
    }

  if ((audio_types & XDP_SCREEN_CAST_AUDIO_TYPE_NONE) &&
      audio_types != XDP_SCREEN_CAST_AUDIO_TYPE_NONE)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Audio type None combined with others: %x", audio_types);
      return FALSE;
    }

  /* Each source type asked for must accept one of the audio types */
  g_variant_lookup (options, "types", "u", &types);
  for (size_t i = 0; i < G_N_ELEMENTS (accepted_audio_types); i++)
    {
      if ((types & accepted_audio_types[i].source_type) &&
          !(audio_types & accepted_audio_types[i].audio_types))
        {
          g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Audio types %x not accepted for source type %x",
                       audio_types, accepted_audio_types[i].source_type);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
validate_exclude_self (const char  *key,
                       GVariant    *value,
                       GVariant    *options,
                       gpointer     user_data,
                       GError     **error)
{
  ScreenCast *screen_cast = user_data;
  uint32_t available =
    xdp_dbus_screen_cast_get_available_exclude_self_modes (XDP_DBUS_SCREEN_CAST (screen_cast));
  uint32_t modes = g_variant_get_uint32 (value);

  if (modes == 0 || (modes & ~available) != 0)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unavailable self exclusion modes %x", modes);
      return FALSE;
    }

  if ((modes & XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_NONE) &&
      modes != XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_NONE)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Self exclusion mode None combined with others: %x", modes);
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


static XdpOptionKey screen_cast_select_sources_options[] = {
  { "types", G_VARIANT_TYPE_UINT32, validate_source_types },
  { "multiple", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "cursor_mode", G_VARIANT_TYPE_UINT32, validate_cursor_mode },
  { "restore_token", G_VARIANT_TYPE_STRING, validate_restore_token },
  { "persist_mode", G_VARIANT_TYPE_UINT32, validate_persist_mode },
  { "audio_types", G_VARIANT_TYPE_UINT32, validate_audio_types },
  { "exclude_self", G_VARIANT_TYPE_UINT32, validate_exclude_self },
};

static gboolean
replace_screen_cast_restore_token_with_data (XdpSession  *session,
                                             GVariant   **in_out_options,
                                             GError     **error)
{
  XdpSessionPersistenceMode persist_mode;

  if (!g_variant_lookup (*in_out_options, "persist_mode", "u", &persist_mode))
    persist_mode = XDP_SESSION_PERSISTENCE_MODE_NONE;

  if (IS_REMOTE_DESKTOP_SESSION (session))
    {
      if (persist_mode != XDP_SESSION_PERSISTENCE_MODE_NONE ||
          xdp_variant_contains_key (*in_out_options, "restore_token"))
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
                                                               SCREEN_CAST_PERMISSION_TABLE,
                                                               in_out_options,
                                                               &screen_cast_session->restore_token);
    }

  return TRUE;
}

static gboolean
handle_select_sources (XdpDbusScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  ScreenCast *screen_cast = (ScreenCast *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  uint32_t audio_types;
  uint32_t exclude_self;

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

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (screen_cast->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (screen_cast->impl)),
    xdp_request_get_object_path (request),
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
                           screen_cast, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!g_variant_lookup (arg_options, "audio_types", "u", &audio_types))
    audio_types = XDP_SCREEN_CAST_AUDIO_TYPE_NONE;
  if (IS_SCREEN_CAST_SESSION (session))
    SCREEN_CAST_SESSION (session)->audio_types = audio_types;
  else
    remote_desktop_session_set_audio_types (REMOTE_DESKTOP_SESSION (session), audio_types);

  if (!g_variant_lookup (arg_options, "exclude_self", "u", &exclude_self))
    exclude_self = XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_NONE;

  if (exclude_self & XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_AUDIO)
    {
      const char *app_id = xdp_app_info_get_id (request->app_info);

      if (!app_id || *app_id == '\0')
        {
          g_dbus_method_invocation_return_error (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                                 "The application can't be identified to leave out its audio");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  if (IS_SCREEN_CAST_SESSION (session))
    SCREEN_CAST_SESSION (session)->exclude_self = exclude_self;
  else
    remote_desktop_session_set_exclude_self (REMOTE_DESKTOP_SESSION (session), exclude_self);

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

  xdp_dbus_impl_screen_cast_call_select_sources (screen_cast->impl,
                                                 xdp_request_get_object_path (request),
                                                 arg_session_handle,
                                                 xdp_app_info_get_id (request->app_info),
                                                 options,
                                                 NULL,
                                                 select_sources_done,
                                                 g_object_ref (request));

  xdp_dbus_screen_cast_complete_select_sources (object, invocation, xdp_request_get_object_path (request));

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

  pipewire_properties = pw_properties_new (XDP_PW_KEY_APP_ID, app_id,
                                           XDP_PW_KEY_MEDIA_ROLES, "",
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
  GVariant *options;

  while (g_variant_iter_next (streams_iter, "(u@a{sv})",
                              &stream_id, &options))
    {
      g_autoptr(GVariant) stream_options = options;
      ScreenCastStream *stream;

      stream = g_new0 (ScreenCastStream, 1);
      stream->id = stream_id;
      g_variant_lookup (stream_options, "size", "(ii)",
                        &stream->width, &stream->height);
      stream->has_pipewire_serial =
        g_variant_lookup (stream_options, "pipewire-serial", "t",
                          &stream->pipewire_serial);

      streams = g_list_prepend (streams, stream);
    }

  return streams;
}

G_DEFINE_QUARK (xdp-screen-cast-audio-session, audio_session)
G_DEFINE_QUARK (xdp-screen-cast-app-audio-capture, app_audio_capture)

static void
on_audio_session_closed (XdpDbusImplSession *audio_session,
                         XdpSession         *session)
{
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  g_object_set_qdata (G_OBJECT (session), audio_session_quark (), NULL);
  xdp_session_close (session, TRUE);
}

static gboolean
ensure_audio_session (XdpSession  *session,
                      uint32_t     exclude_self,
                      GError     **error)
{
  const char *handle = xdp_session_get_object_path (session);
  g_auto(GVariantBuilder) options = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(XdpDbusImplAppAudioCapture) app_audio_capture = NULL;
  g_autoptr(XdpDbusImplSession) audio_session = NULL;

  if (g_object_get_qdata (G_OBJECT (session), audio_session_quark ()))
    return TRUE;

  app_audio_capture =
    xdp_dbus_impl_app_audio_capture_proxy_new_sync (session->impl_connection,
                                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                    APP_AUDIO_CAPTURE_DBUS_IMPL_IFACE,
                                                    DESKTOP_DBUS_PATH,
                                                    NULL, error);
  if (!app_audio_capture)
    return FALSE;

  audio_session = xdp_dbus_impl_session_proxy_new_sync (session->impl_connection,
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                        APP_AUDIO_CAPTURE_DBUS_IMPL_IFACE,
                                                        handle,
                                                        NULL, error);
  if (!audio_session)
    return FALSE;
  g_signal_connect_object (audio_session, "closed",
                           G_CALLBACK (on_audio_session_closed), session, G_CONNECT_DEFAULT);

  if (exclude_self & XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_AUDIO)
    g_variant_builder_add (&options, "{sv}", "exclude_self", g_variant_new_boolean (TRUE));
  if (!xdp_dbus_impl_app_audio_capture_call_create_session_sync (
        app_audio_capture, handle, xdp_app_info_get_id (session->app_info),
        g_variant_builder_end (&options), NULL, error))
    return FALSE;

  g_object_set_qdata_full (G_OBJECT (session), app_audio_capture_quark (),
                           g_steal_pointer (&app_audio_capture), g_object_unref);
  g_object_set_qdata_full (G_OBJECT (session), audio_session_quark (),
                           g_steal_pointer (&audio_session), g_object_unref);
  return TRUE;
}

static gboolean
is_caller_application (XdpSession *session,
                       pid_t       pid)
{
  g_autoptr(XdpAppInfo) app_info = NULL;

  if (pid <= 0)
    return FALSE;

  app_info = xdp_app_info_new (NULL, pid, -1, NULL);
  return app_info &&
         g_strcmp0 (xdp_app_info_get_id (app_info),
                    xdp_app_info_get_id (session->app_info)) == 0;
}

static gboolean
create_audio_stream (XdpSession       *session,
                     uint32_t          audio_types,
                     uint32_t          exclude_self,
                     uint32_t          source_type,
                     GVariant         *audio_source,
                     const char       *mapping_id,
                     GVariantBuilder  *streams_builder,
                     GList           **streams,
                     GError          **error)
{
  g_auto(GVariantBuilder) options = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *handle = xdp_session_get_object_path (session);
  g_autoptr(GVariant) results = NULL;
  XdpDbusImplAppAudioCapture *app_audio_capture;
  gboolean created;
  ScreenCastStream *stream;
  uint32_t node_id;
  uint64_t serial;
  uint32_t audio_type = 0;
  uint32_t pid = 0;

  /* The backend picks it, among the types the caller accepts for the source */
  if (!g_variant_lookup (audio_source, "audio_type", "u", &audio_type) ||
      (audio_type != XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM &&
       audio_type != XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION) ||
      !(audio_type & audio_types & accepted_audio_types_for (source_type)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Audio type %x not asked for source type %x", audio_type, source_type);
      return FALSE;
    }

  if (audio_type == XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION &&
      g_variant_lookup (audio_source, "pid", "u", &pid))
    g_variant_builder_add (&options, "{sv}", "pid", g_variant_new_uint32 (pid));
  /* A window of the caller's own application gets no audio stream */
  if ((exclude_self & XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_AUDIO) &&
      audio_type == XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION &&
      is_caller_application (session, pid))
    return TRUE;

  if (!ensure_audio_session (session, exclude_self, error))
    return FALSE;

  app_audio_capture = g_object_get_qdata (G_OBJECT (session), app_audio_capture_quark ());
  if (audio_type == XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION)
    created = xdp_dbus_impl_app_audio_capture_call_create_application_stream_sync (
      app_audio_capture, handle, g_variant_builder_end (&options), &results, NULL, error);
  else
    created = xdp_dbus_impl_app_audio_capture_call_create_system_stream_sync (
      app_audio_capture, handle, g_variant_builder_end (&options), &results, NULL, error);
  if (!created)
    return FALSE;

  if (!g_variant_lookup (results, "pipewire-node-id", "u", &node_id) ||
      !g_variant_lookup (results, "pipewire-serial", "t", &serial))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No audio stream node");
      return FALSE;
    }

  stream = g_new0 (ScreenCastStream, 1);
  stream->id = node_id;
  stream->pipewire_serial = serial;
  stream->has_pipewire_serial = TRUE;
  *streams = g_list_append (*streams, stream);

  g_variant_builder_add (streams_builder, "(u@a{sv})", node_id,
                         g_variant_new_parsed ("{'media_type': <'audio'>, "
                                               "'audio_type': <%u>, "
                                               "'mapping_id': <%s>, "
                                               "'pipewire-serial': <%t>}",
                                               audio_type, mapping_id, serial));
  return TRUE;
}

gboolean
screen_cast_create_audio_streams (XdpSession   *session,
                                  uint32_t      audio_types,
                                  uint32_t      exclude_self,
                                  GVariant    **in_out_results,
                                  GList       **streams,
                                  GError      **error)
{
  g_auto(GVariantBuilder) results = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_auto(GVariantBuilder) streams_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(ua{sv})"));
  g_autoptr(GVariantIter) streams_iter = NULL;
  GVariantIter results_iter;
  const char *key;
  GVariant *value;
  uint32_t node_id;
  GVariant *options;

  if (!g_variant_lookup (*in_out_results, "streams", "a(ua{sv})", &streams_iter))
    return TRUE;

  while (g_variant_iter_next (streams_iter, "(u@a{sv})", &node_id, &options))
    {
      g_autoptr(GVariant) stream_options = options;
      g_autoptr(GVariant) audio_source = NULL;
      g_autofree char *mapping_id = NULL;
      uint32_t source_type = 0;
      g_auto(GVariantBuilder) video = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      GVariantIter options_iter;

      audio_source = g_variant_lookup_value (stream_options, "audio_source",
                                             G_VARIANT_TYPE_VARDICT);
      if (!audio_source)
        {
          g_variant_builder_add (&streams_builder, "(u@a{sv})", node_id, stream_options);
          continue;
        }

      if (!g_variant_lookup (stream_options, "mapping_id", "s", &mapping_id))
        mapping_id = g_uuid_string_random ();
      g_variant_lookup (stream_options, "source_type", "u", &source_type);

      g_variant_iter_init (&options_iter, stream_options);
      while (g_variant_iter_next (&options_iter, "{&sv}", &key, &value))
        {
          g_autoptr(GVariant) option = value;

          if (!g_str_equal (key, "audio_source") && !g_str_equal (key, "mapping_id"))
            g_variant_builder_add (&video, "{sv}", key, option);
        }
      g_variant_builder_add (&video, "{sv}", "mapping_id", g_variant_new_string (mapping_id));
      g_variant_builder_add (&streams_builder, "(u@a{sv})", node_id,
                             g_variant_builder_end (&video));

      /* Closed while the backend's Start was in flight: nothing would close the stream */
      if (session->closed)
        continue;

      if (!create_audio_stream (session, audio_types, exclude_self, source_type, audio_source,
                                mapping_id, &streams_builder, streams, error))
        return FALSE;
    }

  g_variant_iter_init (&results_iter, *in_out_results);
  while (g_variant_iter_next (&results_iter, "{&sv}", &key, &value))
    {
      g_autoptr(GVariant) result = value;

      if (!g_str_equal (key, "streams"))
        g_variant_builder_add (&results, "{sv}", key, result);
    }
  g_variant_builder_add (&results, "{sv}", "streams", g_variant_builder_end (&streams_builder));

  g_variant_unref (*in_out_results);
  *in_out_results = g_variant_ref_sink (g_variant_builder_end (&results));
  return TRUE;
}

void
screen_cast_close_audio_session (XdpSession *session)
{
  g_autoptr(XdpDbusImplSession) audio_session =
    g_object_steal_qdata (G_OBJECT (session), audio_session_quark ());

  if (!audio_session)
    return;

  g_signal_handlers_disconnect_by_data (audio_session, session);
  xdp_dbus_impl_session_call_close (audio_session, NULL, NULL, NULL);
}

static void
replace_restore_screen_cast_data_with_token (ScreenCastSession *screen_cast_session,
                                             GVariant **in_out_results)
{
  xdp_session_persistence_replace_restore_data_with_token (XDP_SESSION (screen_cast_session),
                                                           SCREEN_CAST_PERMISSION_TABLE,
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
  XdpDbusImplScreenCast *impl = (XdpDbusImplScreenCast *) source_object;
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
          if (!process_results (screen_cast_session, &results, &error) ||
              !screen_cast_create_audio_streams (session, screen_cast_session->audio_types,
                                                 screen_cast_session->exclude_self,
                                                 &results,
                                                 &screen_cast_session->streams, &error))
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
  ScreenCast *screen_cast = (ScreenCast *) object;
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

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (screen_cast->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (screen_cast->impl)),
    xdp_request_get_object_path (request),
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

  xdp_dbus_impl_screen_cast_call_start (screen_cast->impl,
                                        xdp_request_get_object_path (request),
                                        arg_session_handle,
                                        xdp_app_info_get_id (request->app_info),
                                        arg_parent_window,
                                        options,
                                        NULL,
                                        start_done,
                                        g_object_ref (request));

  xdp_dbus_screen_cast_complete_start (object, invocation, xdp_request_get_object_path (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_open_pipewire_remote (XdpDbusScreenCast *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *in_fd_list,
                             const char *arg_session_handle,
                             GVariant *arg_options)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  XdpSession *session;
  GList *streams;
  PipeWireRemote *remote;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int fd;
  int fd_id;
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
screen_cast_dispose (GObject *object)
{
  ScreenCast *screen_cast = (ScreenCast *) object;

  g_clear_object (&screen_cast->impl);

  G_OBJECT_CLASS (screen_cast_parent_class)->dispose (object);
}

static void
screen_cast_init (ScreenCast *screen_cast)
{
}

static void
screen_cast_class_init (ScreenCastClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = screen_cast_dispose;

  quark_request_session =
    g_quark_from_static_string ("-xdp-request-screen-cast-session");
}

static gboolean
add_audio_type_none (GBinding     *binding,
                     const GValue *from_value,
                     GValue       *to_value,
                     gpointer      user_data)
{
  g_value_set_uint (to_value,
                    XDP_SCREEN_CAST_AUDIO_TYPE_NONE |
                    (g_value_get_uint (from_value) &
                     (XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM | XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION)));
  return TRUE;
}

static ScreenCast *
screen_cast_new (XdpContext            *context,
                 XdpDbusImplScreenCast *impl)
{
  ScreenCast *screen_cast;

  screen_cast = g_object_new (screen_cast_get_type (), NULL);
  screen_cast->context = context;
  screen_cast->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (screen_cast->impl), G_MAXINT);

  xdp_dbus_screen_cast_set_version (XDP_DBUS_SCREEN_CAST (screen_cast),
                                    MIN (xdp_dbus_impl_screen_cast_get_version (screen_cast->impl), 6));

  g_object_bind_property (G_OBJECT (screen_cast->impl), "available-source-types",
                          G_OBJECT (screen_cast), "available-source-types",
                          G_BINDING_SYNC_CREATE);

  if (xdp_dbus_impl_screen_cast_get_version (screen_cast->impl) >= 2)
    {
      g_object_bind_property (G_OBJECT (screen_cast->impl), "available-cursor-modes",
                              G_OBJECT (screen_cast), "available-cursor-modes",
                              G_BINDING_SYNC_CREATE);
    }

  xdp_dbus_screen_cast_set_available_audio_types (XDP_DBUS_SCREEN_CAST (screen_cast),
                                                  XDP_SCREEN_CAST_AUDIO_TYPE_NONE);
  if (xdp_dbus_impl_screen_cast_get_version (screen_cast->impl) >= 7)
    {
      g_object_bind_property_full (G_OBJECT (screen_cast->impl), "available-audio-types",
                                   G_OBJECT (screen_cast), "available-audio-types",
                                   G_BINDING_SYNC_CREATE,
                                   add_audio_type_none, NULL, NULL, NULL);
    }
  xdp_dbus_screen_cast_set_available_exclude_self_modes (XDP_DBUS_SCREEN_CAST (screen_cast),
                                                         XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_NONE |
                                                         XDP_SCREEN_CAST_EXCLUDE_SELF_MODE_AUDIO);

  return screen_cast;
}

void
init_screen_cast (XdpContext *context)
{
  g_autoptr(ScreenCast) screen_cast = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplScreenCast) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SCREEN_CAST_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_screen_cast_proxy_new_sync (connection,
                                                   G_DBUS_PROXY_FLAGS_NONE,
                                                   impl_config->dbus_name,
                                                   DESKTOP_DBUS_PATH,
                                                   NULL,
                                                   &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create screen cast proxy: %s", error->message);
      return;
    }

  screen_cast = screen_cast_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&screen_cast)),
                                      XDP_ENTITLEMENT_SCREEN_CAST,
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);
}
