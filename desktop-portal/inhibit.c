/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "inhibit.h"

#include <string.h>

#include <gio/gio.h>

#include <systemd/sd-login.h>
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-session.h"
#include "xdp-types.h"
#include "xdp-utils.h"

enum {
  INHIBIT_LOGOUT       = 1,
  INHIBIT_USER_SWITCH  = 2,
  INHIBIT_SUSPEND      = 4,
  INHIBIT_IDLE         = 8
};

#define INHIBIT_ALL (INHIBIT_LOGOUT|INHIBIT_USER_SWITCH|INHIBIT_SUSPEND|INHIBIT_IDLE)

enum {
  STATE_RUNNING    = 1,
  STATE_QUERY_END  = 2,
  STATE_ENDING     = 3,
  STATE_SUSPENDING = 4,
};

#define LOGIN1_NAME "org.freedesktop.login1"
#define LOGIN1_MANAGER_INTERFACE "org.freedesktop.login1.Manager"
#define LOGIN1_MANAGER_PATH "/org/freedesktop/login1"
#define LOGIN1_SESSION_INTERFACE "org.freedesktop.login1.Session"

typedef struct _Inhibit Inhibit;
typedef struct _InhibitClass InhibitClass;

struct _Inhibit
{
  XdpDbusInhibitSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplInhibit *impl;

  GDBusProxy *logind_proxy;
  GDBusProxy *logind_session_proxy;

  /* Any of the state below can be accessed across threads (especially the
   * suspend delay inhibitor and timeout callback), so it's all locked behind
   * a (this) mutex.
   */
  GMutex mutex;

  /* This holds the last session state we got from the implementation.
   * This should never hold Suspending, since we fire it ourselves.
   * Re-send this as the session state once we get PrepareForSleep(false).
   */
  guint32 last_session_state;
  GHashTable *sessions;
  int suspend_delay_inhibitor_fd;
  guint ones_timeout_callback_id;
};

struct _InhibitClass
{
  XdpDbusInhibitSkeletonClass parent_class;
};

GType inhibit_get_type (void);
static void inhibit_iface_init (XdpDbusInhibitIface *iface);

G_DEFINE_TYPE_WITH_CODE (Inhibit, inhibit, XDP_DBUS_TYPE_INHIBIT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_INHIBIT,
                                                inhibit_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Inhibit, g_object_unref)

static guint32
get_allowed_inhibit (XdpAppInfo *app_info)
{
  g_auto(GStrv) perms = NULL;
  guint32 ret = 0;

  perms = xdp_get_permissions_sync (app_info,
                                    INHIBIT_PERMISSION_TABLE,
                                    INHIBIT_PERMISSION_ID);

  if (perms != NULL)
    {
      int i;

      for (i = 0; perms[i]; i++)
        {
          if (g_strcmp0 (perms[i], "logout") == 0)
            ret |= INHIBIT_LOGOUT;
          else if (g_strcmp0 (perms[i], "switch") == 0)
            ret |= INHIBIT_USER_SWITCH;
          else if (g_strcmp0 (perms[i], "suspend") == 0)
            ret |= INHIBIT_SUSPEND;
          else if (g_strcmp0 (perms[i], "idle") == 0)
            ret |= INHIBIT_IDLE;
          else
            g_warning ("Unknown inhibit flag in permission store: %s", perms[i]);
        }
    }
  else
    ret = INHIBIT_ALL; /* all allowed */

  g_debug ("Inhibit permissions for %s: %d",
           xdp_app_info_get_id (app_info), ret);

  return ret;
}

static gboolean
validate_reason (const char  *key,
                 GVariant    *value,
                 GVariant    *options,
                 gpointer     user_data,
                 GError     **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (g_utf8_strlen (string, -1) > 256)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting overly long reasons");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey inhibit_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason }
};

static gboolean
handle_inhibit (XdpDbusInhibit *object,
                GDBusMethodInvocation *invocation,
                const char *arg_window,
                guint32 arg_flags,
                GVariant *arg_options)
{
  Inhibit *inhibit = (Inhibit *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  const char *app_id;
  guint32 flags;
  XdgDesktopPortalResponseEnum response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;

  REQUEST_AUTOLOCK (request);

  if ((arg_flags & ~INHIBIT_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid flags");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_filter_options (arg_options, &opt_builder,
                      inhibit_options, G_N_ELEMENTS (inhibit_options),
                      NULL, NULL);

  options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

  impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl)),
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                       g_dbus_proxy_get_name (G_DBUS_PROXY (inhibit->impl)),
                                                       xdp_request_get_object_path (request),
                                                       NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);

  app_id = xdp_app_info_get_id (request->app_info);
  flags = arg_flags & get_allowed_inhibit (request->app_info);

  /* The backend exports its request object while it handles the call, so the
   * request is exported only once the call returned. Otherwise the caller
   * could close the request before the backend has it, closing it in the
   * backend would fail, and the inhibition would stay until the caller
   * disconnects. */
  if (flags != 0)
    {
      g_debug ("Calling inhibit backend for %s: %d", app_id, flags);
      if (!xdp_dbus_impl_inhibit_call_inhibit_sync (inhibit->impl,
                                                    xdp_request_get_object_path (request),
                                                    app_id,
                                                    arg_window,
                                                    flags,
                                                    options,
                                                    NULL,
                                                    &error))
        {
          g_dbus_error_strip_remote_error (error);
          g_warning ("A backend call failed: %s", error->message);
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        }
    }

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_inhibit_complete_inhibit (object, invocation, xdp_request_get_object_path (request));

  if (flags != 0)
    {
      g_auto(GVariantBuilder) results =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&results));
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

typedef struct _InhibitSession
{
  XdpSession parent;

  Inhibit *inhibit;
  gboolean suspend_aware;
  gboolean pending_suspend_response;
  gboolean closed;
} InhibitSession;

typedef struct _InhibitSessionClass
{
  XdpSessionClass parent_class;
} InhibitSessionClass;

GType inhibit_session_get_type (void);

G_DEFINE_TYPE (InhibitSession, inhibit_session, xdp_session_get_type ())

G_DEFINE_AUTOPTR_CLEANUP_FUNC (InhibitSession, g_object_unref)

static inline InhibitSession *
INHIBIT_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, inhibit_session_get_type (), InhibitSession);
}

static inline gboolean
IS_INHIBIT_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, inhibit_session_get_type ());
}

static void
inhibit_session_close (XdpSession *session)
{
  InhibitSession *inhibit_session = INHIBIT_SESSION (session);
  Inhibit *inhibit = inhibit_session->inhibit;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);

  inhibit_session->closed = TRUE;

  g_debug ("inhibit session owned by '%s' closed", session->sender);

  g_hash_table_remove (inhibit->sessions, session);
}

static void
inhibit_session_init (InhibitSession *inhibit_session)
{
}

static void
inhibit_session_class_init (InhibitSessionClass *klass)
{
  XdpSessionClass *session_class;

  session_class = (XdpSessionClass *)klass;
  session_class->close = inhibit_session_close;
}

static InhibitSession *
inhibit_session_new (Inhibit     *inhibit,
                     GVariant    *options,
                     XdpRequest  *request,
                     GError     **error)
{
  InhibitSession *inhibit_session;
  const char *session_token;
  GDBusInterfaceSkeleton *interface_skeleton = G_DBUS_INTERFACE_SKELETON (request);
  GDBusConnection *connection = g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (inhibit->impl));
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);

  session_token = lookup_session_token (options);
  inhibit_session = g_initable_new (inhibit_session_get_type (), NULL, error,
                                    "context", inhibit->context,
                                    "app-info", request->app_info,
                                    "token", session_token,
                                    "connection", connection,
                                    "impl-connection", impl_connection,
                                    "impl-dbus-name", impl_dbus_name,
                                    NULL);

  if (!inhibit_session)
    return NULL;

  g_debug ("inhibit session owned by '%s' created", inhibit_session->parent.sender);

  g_variant_lookup (options, "suspend_aware", "b", &inhibit_session->suspend_aware);

  inhibit_session->inhibit = inhibit;
  g_hash_table_add (inhibit->sessions, inhibit_session);

  return inhibit_session;
}

static void
emit_state_changed (GDBusConnection *connection,
                    InhibitSession  *inhibit_session,
                    GVariant        *state)
{
  XdpSession *session = &inhibit_session->parent;

  g_dbus_connection_emit_signal (connection,
                                 session->sender,
                                 DESKTOP_DBUS_PATH,
                                 INHIBIT_DBUS_IFACE,
                                 "StateChanged",
                                 g_variant_new ("(o@a{sv})",
                                                xdp_session_get_object_path (session),
                                                state),
                                 NULL);
}

static void
create_monitor_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  XdpDbusImplInhibit *impl = (XdpDbusImplInhibit *) source_object;
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(XdpRequest) request = data;
  XdpSession *session;
  guint response = 2;
  gboolean should_close_session;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) locked_hint = NULL;
  g_autoptr(GVariant) state = NULL;
  GVariantBuilder state_builder;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_data (G_OBJECT (request), "session");
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_data (G_OBJECT (request), "session", NULL);

  if (!xdp_dbus_impl_inhibit_call_create_monitor_finish (impl, &response, res, &error))
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
  else if (INHIBIT_SESSION (session)->inhibit->logind_session_proxy)
    {
      locked_hint =
        g_dbus_proxy_get_cached_property (INHIBIT_SESSION (session)->inhibit->logind_session_proxy,
                                          "LockedHint");

      if (!locked_hint)
        return;

      g_variant_builder_init (&state_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&state_builder, "{sv}",
                             "locked", locked_hint);
      state = g_variant_ref_sink (g_variant_builder_end (&state_builder));

      emit_state_changed (connection, INHIBIT_SESSION (session), state);
    }
}

static gboolean
handle_create_monitor (XdpDbusInhibit *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_window,
                       GVariant *arg_options)
{
  Inhibit *inhibit = (Inhibit *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  XdpSession *session;

  REQUEST_AUTOLOCK (request);

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (inhibit->impl)),
                                          xdp_request_get_object_path (request),
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = XDP_SESSION (inhibit_session_new (inhibit, arg_options, request, &error));
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (request), "session", g_object_ref (session), g_object_unref);

  xdp_dbus_impl_inhibit_call_create_monitor (inhibit->impl,
                                             xdp_request_get_object_path (request),
                                             xdp_session_get_object_path (session),
                                             xdp_app_info_get_id (request->app_info),
                                             arg_window,
                                             NULL,
                                             create_monitor_done,
                                             g_object_ref (request));

  xdp_dbus_inhibit_complete_create_monitor (object, invocation, xdp_request_get_object_path (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_query_end_response (XdpDbusInhibit        *object,
                           GDBusMethodInvocation *invocation,
                           const char            *session_id)
{
  Inhibit *inhibit = (Inhibit *) object;
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);

  if (!session || !IS_INHIBIT_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_inhibit_call_query_end_response (inhibit->impl, xdp_session_get_object_path (session),
                                                 NULL, NULL, NULL);
  xdp_dbus_inhibit_complete_query_end_response (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
close_suspend_inhibitor_timeout (gpointer data)
{
  Inhibit *inhibit = data;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);

  if (g_source_is_destroyed (g_main_current_source ()))
    return G_SOURCE_REMOVE;

  inhibit->ones_timeout_callback_id = 0;
  g_debug ("Inhibit suspend timeout reached; 1 second wait is over");
  g_clear_fd (&inhibit->suspend_delay_inhibitor_fd, NULL);

  return G_SOURCE_REMOVE;
}

static void
maybe_suspend (Inhibit *inhibit)
{
  GHashTableIter iter;
  InhibitSession *inhibit_session = NULL;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);

  g_hash_table_iter_init (&iter, inhibit->sessions);
  while (g_hash_table_iter_next (&iter, (gpointer *) &inhibit_session, NULL))
    if (inhibit_session && !inhibit_session->closed && inhibit_session->suspend_aware)
      if (inhibit_session->pending_suspend_response)
        return;

  g_clear_handle_id (&inhibit->ones_timeout_callback_id, g_source_remove);
  g_clear_fd (&inhibit->suspend_delay_inhibitor_fd, NULL);
}

static gboolean
handle_suspending_response (XdpDbusInhibit        *object,
                            GDBusMethodInvocation *invocation,
                            const char            *session_id)
{
  Inhibit *inhibit = (Inhibit *) object;
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);
  InhibitSession *inhibit_session = (InhibitSession *)session;

  if (!session || !IS_INHIBIT_SESSION (session) ||
      !inhibit_session->suspend_aware ||
      !inhibit_session->pending_suspend_response)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  inhibit_session->pending_suspend_response = FALSE;
  maybe_suspend (inhibit);

  xdp_dbus_inhibit_complete_suspending_response (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
inhibit_iface_init (XdpDbusInhibitIface *iface)
{
  iface->handle_inhibit = handle_inhibit;
  iface->handle_create_monitor = handle_create_monitor;
  iface->handle_query_end_response = handle_query_end_response;
  iface->handle_suspending_response = handle_suspending_response;
}

static void
inhibit_dispose (GObject *object)
{
  Inhibit *inhibit = (Inhibit *) object;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);

  g_clear_handle_id (&inhibit->ones_timeout_callback_id, g_source_remove);
  g_clear_fd (&inhibit->suspend_delay_inhibitor_fd, NULL);
  g_clear_pointer (&inhibit->sessions, g_hash_table_unref);

  g_clear_object (&inhibit->impl);
  g_clear_object (&inhibit->logind_proxy);
  g_clear_object (&inhibit->logind_session_proxy);

  G_OBJECT_CLASS (inhibit_parent_class)->dispose (object);
}

static void
inhibit_finalize (GObject *object)
{
  Inhibit *inhibit = (Inhibit *) object;

  g_mutex_clear (&inhibit->mutex);

  G_OBJECT_CLASS (inhibit_parent_class)->finalize (object);
}

static void
inhibit_init (Inhibit *inhibit)
{
  g_mutex_init (&inhibit->mutex);
}

static void
inhibit_class_init (InhibitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = inhibit_dispose;
  object_class->finalize = inhibit_finalize;
}

static void
inhibit_get_suspend_delay_inhibitor (Inhibit *inhibit)
{
  GVariant *suspend_delay_params = NULL;
  g_autoptr(GVariant) suspend_delay_result = NULL;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int fd_index = 0;
  g_autoptr(GError) error = NULL;

  // We already have an inhibitor.
  if (inhibit->suspend_delay_inhibitor_fd >= 0)
    return;

  // (what, who, why, mode)
  suspend_delay_params = g_variant_new ("(ssss)", "sleep",
                                        "XDG Desktop Portal",
                                        "Inhibit portal suspending signal",
                                        "delay");
  suspend_delay_result =
    g_dbus_proxy_call_with_unix_fd_list_sync (inhibit->logind_proxy, "Inhibit",
                                              suspend_delay_params,
                                              G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                              &out_fd_list, NULL, &error);
  if (suspend_delay_result == NULL)
    {
      g_warning ("Failed to get suspend delay inhibitor: %s", error->message);
      return;
    }

  g_variant_get (suspend_delay_result, "(h)", &fd_index);

  inhibit->suspend_delay_inhibitor_fd = g_unix_fd_list_get (out_fd_list,
                                                            fd_index,
                                                            &error);

  if (inhibit->suspend_delay_inhibitor_fd < 0)
      g_warning ("Failed to get suspend delay inhibitor fd: %s", error->message);
}

static void
on_state_changed (XdpDbusImplInhibit *impl,
                  const char         *session_id,
                  GVariant           *state,
                  gpointer            data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);
  InhibitSession *inhibit_session = INHIBIT_SESSION (session);
  Inhibit *inhibit = data;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);
  gboolean active = FALSE;
  guint32 session_state = 0;

  g_variant_lookup (state, "screensaver-active", "b", &active);
  g_variant_lookup (state, "session-state", "u", &session_state);
  g_debug ("Received state-changed %s: screensaver-active: %d, session-state: %u",
           session_id, active, session_state);

  if (session_state && session_state != STATE_SUSPENDING)
    inhibit->last_session_state = session_state;

  if (inhibit_session && !inhibit_session->closed)
    emit_state_changed (connection, inhibit_session, state);
}

static void
on_logind_proxy_signal (GDBusProxy *proxy,
                        gchar      *sender_name,
                        gchar      *signal_name,
                        GVariant   *parameters,
                        gpointer    user_data)
{
  Inhibit *inhibit = user_data;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);
  GDBusConnection *connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl));
  gboolean should_prepare_for_sleep = FALSE;
  g_autoptr(GVariant) state = NULL;
  GVariantBuilder state_builder;
  guint32 session_state = 0;
  InhibitSession *inhibit_session;
  GHashTableIter iter;

  if (g_strcmp0 (signal_name, "PrepareForSleep") == 0)
    {
      g_variant_get (parameters, "(b)", &should_prepare_for_sleep);
      g_debug ("PrepareForSleep fired with %s",
               should_prepare_for_sleep ? "true" : "false");

      if (should_prepare_for_sleep)
          session_state = STATE_SUSPENDING;
      else // coming back from sleep
        {
          inhibit_get_suspend_delay_inhibitor (inhibit);
          session_state = inhibit->last_session_state;
          // We might've gotten suspended before this fired, so cancel it.
          g_clear_handle_id (&inhibit->ones_timeout_callback_id,
                             g_source_remove);
        }

        g_variant_builder_init (&state_builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add (&state_builder, "{sv}",
                               "session-state",
                               g_variant_new_uint32 (session_state));
        state = g_variant_ref_sink (g_variant_builder_end (&state_builder));

        gboolean has_suspend_aware_session = FALSE;
        g_hash_table_iter_init (&iter, inhibit->sessions);
        while (g_hash_table_iter_next (&iter, (gpointer *) &inhibit_session, NULL))
          if (inhibit_session && !inhibit_session->closed && inhibit_session->suspend_aware)
            {
              has_suspend_aware_session = TRUE;
              inhibit_session->pending_suspend_response = should_prepare_for_sleep;
              emit_state_changed (connection, inhibit_session, state);
            }

        if (should_prepare_for_sleep)
          {
            if (!has_suspend_aware_session)
              {
                g_clear_handle_id (&inhibit->ones_timeout_callback_id,
                                   g_source_remove);
                g_clear_fd (&inhibit->suspend_delay_inhibitor_fd, NULL);
              }
            else if (!inhibit->ones_timeout_callback_id)
              inhibit->ones_timeout_callback_id =
                g_timeout_add (1000, close_suspend_inhibitor_timeout, inhibit);
          }
    }
}

static void
on_logind_session_proxy_properties_changed (GDBusProxy  *proxy,
                                            GVariant    *changed_properties,
                                            GStrv        invalidated_properties,
                                            gpointer     user_data)
{
  Inhibit *inhibit = user_data;
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&inhibit->mutex);
  GDBusConnection *connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl));
  InhibitSession *inhibit_session;
  g_autoptr(GVariant) state = NULL;
  GHashTableIter iter;
  gboolean locked_hint = FALSE;
  GVariantBuilder state_builder;

  if (!g_variant_lookup (changed_properties, "LockedHint", "b", &locked_hint))
    return;

  g_debug ("LockedHint changed: %s", locked_hint ? "true" : "false");

  g_variant_builder_init (&state_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&state_builder, "{sv}",
                         "locked", g_variant_new_boolean (locked_hint));
  state = g_variant_ref_sink (g_variant_builder_end (&state_builder));

  g_hash_table_iter_init (&iter, inhibit->sessions);
  while (g_hash_table_iter_next (&iter, (gpointer *) &inhibit_session, NULL))
      if (inhibit_session && !inhibit_session->closed)
        emit_state_changed (connection, inhibit_session, state);
}

static Inhibit *
inhibit_new (XdpContext         *context,
             XdpDbusImplInhibit *impl,
             GDBusProxy         *logind_proxy,
             GDBusProxy         *logind_session_proxy)
{
  Inhibit *inhibit;
  g_autoptr(GMutexLocker) locker = NULL;

  inhibit = g_object_new (inhibit_get_type (), NULL);
  inhibit->context = context;
  inhibit->impl = g_object_ref (impl);

  locker = g_mutex_locker_new (&inhibit->mutex);

  inhibit->last_session_state = STATE_RUNNING;
  inhibit->sessions = g_hash_table_new (g_direct_hash, g_direct_equal);
  inhibit->suspend_delay_inhibitor_fd = -1;
  inhibit->ones_timeout_callback_id = 0;

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (inhibit->impl), G_MAXINT);

  g_signal_connect_object (inhibit->impl, "state-changed",
                           G_CALLBACK (on_state_changed),
                           inhibit, G_CONNECT_DEFAULT);

  if (logind_proxy != NULL)
    {
      inhibit->logind_proxy = g_object_ref (logind_proxy);
      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (inhibit->logind_proxy),
                                        G_MAXINT);
      g_signal_connect_object (inhibit->logind_proxy, "g-signal",
                               G_CALLBACK (on_logind_proxy_signal),
                               inhibit, G_CONNECT_DEFAULT);
      inhibit_get_suspend_delay_inhibitor (inhibit);
  }

  if (logind_session_proxy != NULL)
    {
      inhibit->logind_session_proxy = g_object_ref (logind_session_proxy);
      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (inhibit->logind_session_proxy),
                                        G_MAXINT);
      g_signal_connect_object (inhibit->logind_session_proxy,
                               "g-properties-changed",
                               G_CALLBACK (on_logind_session_proxy_properties_changed),
                               inhibit, G_CONNECT_DEFAULT);
    }

  xdp_dbus_inhibit_set_version (XDP_DBUS_INHIBIT (inhibit), 4);

  return inhibit;
}

void
init_inhibit (XdpContext *context)
{
  g_autoptr(Inhibit) inhibit = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(GDBusProxy) logind_proxy = NULL;
  g_autoptr(GDBusProxy) logind_session_proxy = NULL;
  g_autoptr(XdpDbusImplInhibit) impl = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *logind_session_id = NULL;
  g_autofree gchar *logind_session_path = NULL;
  int sd_return;

  impl_config = xdp_portal_config_find (config, INHIBIT_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_inhibit_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               impl_config->dbus_name,
                                               DESKTOP_DBUS_PATH,
                                               NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create inhibit proxy: %s", error->message);
      return;
    }

  logind_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                NULL, LOGIN1_NAME,
                                                LOGIN1_MANAGER_PATH,
                                                LOGIN1_MANAGER_INTERFACE,
                                                NULL, &error);
  if (logind_proxy == NULL)
    {
      g_warning ("Failed to create logind proxy for inhibit: %s",
                 error->message);
      g_clear_error (&error);
    }

  sd_return = sd_pid_get_session (0, &logind_session_id);
  if (sd_return == -ENODATA)
    {
      sd_return = sd_uid_get_display (getuid(), &logind_session_id);
      if (sd_return < 0)
        g_warning ("Failed to get systemd session ID: %s",
                   strerror (-sd_return));
    }

  if (logind_session_id != NULL)
    {
      const char *prefix = g_ascii_isdigit (logind_session_id[0]) ? "_3" : "";
      logind_session_path = g_strdup_printf ("/org/freedesktop/login1/session/%s%s",
                                             prefix, logind_session_id);

      logind_session_proxy =
        g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                       G_DBUS_PROXY_FLAGS_NONE,
                                       NULL, LOGIN1_NAME,
                                       logind_session_path,
                                       LOGIN1_SESSION_INTERFACE,
                                       NULL, &error);

      if (logind_session_proxy == NULL)
        {
          g_warning ("Failed to create logind session proxy for inhibit: %s",
                     error->message);
          g_clear_error (&error);
        }
    }

  inhibit = inhibit_new (context, impl, logind_proxy, logind_session_proxy);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&inhibit)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);
}
