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
#define LOGIN1_SESSION_PATH_PREFIX "/org/freedesktop/login1/session/"

typedef struct _Inhibit Inhibit;
typedef struct _InhibitClass InhibitClass;

struct _Inhibit
{
  XdpDbusInhibitSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplInhibit *impl;

  GDBusProxy *logind_proxy;
  GDBusProxy *logind_session_proxy;

  /* This holds the last session state we got from the implementation.
   * This should never hold Suspending, since we fire it ourselves.
   * Re-send this as the session state once we get PrepareForSleep(false).
   */
  guint32 last_session_state;
  GHashTable *sessions;
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

static void
inhibit_done (GObject *source,
              GAsyncResult *result,
              gpointer data)
{
  XdpDbusImplInhibit *impl = (XdpDbusImplInhibit *) source;
  g_autoptr(GError) error = NULL;
  XdpRequest *request = data;
  int response = 0;

  REQUEST_AUTOLOCK (request);

  if (!xdp_dbus_impl_inhibit_call_inhibit_finish (impl, result, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      response = 2;
    }

  if (request->exported)
    {
      g_auto(GVariantBuilder) new_results =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&new_results));
    }
}

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

static void
handle_inhibit_in_thread_func (GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  Inhibit *inhibit = (Inhibit *) source_object;
  XdpRequest *request = XDP_REQUEST (task_data);
  const char *window;
  guint32 flags;
  GVariant *options;
  const char *app_id;

  REQUEST_AUTOLOCK (request);

  window = (const char *)g_object_get_data (G_OBJECT (request), "window");
  flags = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (request), "flags"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  app_id = xdp_app_info_get_id (request->app_info);
  flags = flags & get_allowed_inhibit (request->app_info);

  if (flags == 0)
    return;

  g_debug ("Calling inhibit backend for %s: %d", app_id, flags);
  xdp_dbus_impl_inhibit_call_inhibit (inhibit->impl,
                                      xdp_request_get_object_path (request),
                                      app_id,
                                      window,
                                      flags,
                                      options,
                                      NULL,
                                      inhibit_done,
                                      g_object_ref (request));
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
  g_autoptr(GTask) task = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

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

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);
  g_object_set_data (G_OBJECT (request), "flags", GUINT_TO_POINTER (arg_flags));
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

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
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  task = g_task_new (inhibit, NULL, NULL, NULL);
  g_task_set_source_tag (task, handle_inhibit);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_inhibit_in_thread_func);

  xdp_dbus_inhibit_complete_inhibit (object, invocation, xdp_request_get_object_path (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

typedef struct _InhibitSession
{
  XdpSession parent;

  Inhibit *inhibit;
  gboolean closed;
} InhibitSession;

typedef struct _InhibitSessionClass
{
  XdpSessionClass parent_class;
} InhibitSessionClass;

GType inhibit_session_get_type (void);

G_DEFINE_TYPE (InhibitSession, inhibit_session, xdp_session_get_type ())

G_DEFINE_AUTOPTR_CLEANUP_FUNC (InhibitSession, g_object_unref)

G_GNUC_UNUSED static inline InhibitSession *
INHIBIT_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, inhibit_session_get_type (), InhibitSession);
}

G_GNUC_UNUSED static inline gboolean
IS_INHIBIT_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, inhibit_session_get_type ());
}

static void
inhibit_session_close (XdpSession *session)
{
  InhibitSession *inhibit_session = INHIBIT_SESSION (session);
  Inhibit *inhibit = inhibit_session->inhibit;

  inhibit_session->closed = TRUE;

  g_debug ("inhibit session owned by '%s' closed", session->sender);

  g_assert (g_hash_table_contains (inhibit->sessions, session));
  g_hash_table_remove (inhibit->sessions, session);
}

static void
inhibit_session_finalize (GObject *object)
{
  G_OBJECT_CLASS (inhibit_session_parent_class)->finalize (object);
}

static void
inhibit_session_init (InhibitSession *inhibit_session)
{
}

static void
inhibit_session_class_init (InhibitSessionClass *klass)
{
  GObjectClass *object_class;
  XdpSessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = inhibit_session_finalize;

  session_class = (XdpSessionClass *)klass;
  session_class->close = inhibit_session_close;
}

static InhibitSession *
inhibit_session_new (Inhibit     *inhibit,
                     GVariant    *options,
                     XdpRequest  *request,
                     GError     **error)
{
  XdpSession *session;
  const char *session_token;
  GDBusInterfaceSkeleton *interface_skeleton = G_DBUS_INTERFACE_SKELETON (request);
  GDBusConnection *connection = g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (inhibit->impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (inhibit_session_get_type (), NULL, error,
                            "context", inhibit->context,
                            "app-info", request->app_info,
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("inhibit session owned by '%s' created", session->sender);

  ((InhibitSession*)session)->inhibit = inhibit;
  g_hash_table_add (inhibit->sessions, session);

  return (InhibitSession*)session;
}

static void
create_monitor_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  XdpDbusImplInhibit *impl = (XdpDbusImplInhibit *) source_object;
  g_autoptr(XdpRequest) request = data;
  XdpSession *session;
  guint response = 2;
  gboolean should_close_session;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GError) error = NULL;

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

  if (!session)
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
handle_suspending_response (XdpDbusInhibit        *object,
                            GDBusMethodInvocation *invocation,
                            const char            *session_id)
{
  G_GNUC_UNUSED Inhibit *inhibit = (Inhibit *) object;
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  // TODO: release inhibitor delay if all clients have called this?
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

  g_clear_object (&inhibit->impl);
  g_clear_object (&inhibit->logind_proxy);
  g_clear_object (&inhibit->logind_session_proxy);

  g_clear_pointer (&inhibit->sessions, g_hash_table_unref);

  G_OBJECT_CLASS (inhibit_parent_class)->dispose (object);
}

static void
inhibit_init (Inhibit *inhibit)
{
}

static void
inhibit_class_init (InhibitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = inhibit_dispose;
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
  gboolean active = FALSE;
  guint32 session_state = 0;

  g_variant_lookup (state, "screensaver-active", "b", &active);
  g_variant_lookup (state, "session-state", "u", &session_state);
  g_debug ("Received state-changed %s: screensaver-active: %d, session-state: %u",
           session_id, active, session_state);

  inhibit_session->inhibit->last_session_state = session_state;

  if (inhibit_session && !inhibit_session->closed)
    g_dbus_connection_emit_signal (connection,
                                   session->sender,
                                   DESKTOP_DBUS_PATH,
                                   INHIBIT_DBUS_IFACE,
                                   "StateChanged",
                                   g_variant_new ("(o@a{sv})", session_id, state),
                                   NULL);
}

static void
on_logind_proxy_signal (GDBusProxy *proxy,
                        gchar      *sender_name,
                        gchar      *signal_name,
                        GVariant   *parameters,
                        gpointer    user_data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (proxy);
  Inhibit *inhibit = user_data;
  gboolean should_prepare_for_sleep = FALSE;
  g_autoptr(GVariant) state = NULL;
  g_autoptr(GVariant) session_state = NULL;
  InhibitSession *inhibit_session;
  GHashTableIter iter;

  if (g_strcmp0 (signal_name, "PrepareForSleep") == 0)
    {
      g_variant_get (parameters, "(b)", &should_prepare_for_sleep);
      g_debug ("PrepareForSleep fired with %s",
               should_prepare_for_sleep ? "true" : "false");

      if (should_prepare_for_sleep)
        session_state = g_variant_new_uint32 (STATE_SUSPENDING);
        // TODO: add one-second callback to release suspend delay inhibitor
      else
        {
          session_state = g_variant_new_uint32 (inhibit->last_session_state);
          // TODO: create a suspend delay inhibitor
        }

      state =
        g_variant_new ("a{sv}", g_variant_new ("{sv}",
                                               "session-state", session_state));

      g_hash_table_iter_init (&iter, inhibit->sessions);
      while (g_hash_table_iter_next (&iter, (gpointer *) &inhibit_session, NULL))
          if (inhibit_session && !inhibit_session->closed)
            g_dbus_connection_emit_signal (connection,
                                           // is this the right argument here?
                                           inhibit_session->parent.sender,
                                           DESKTOP_DBUS_PATH,
                                           INHIBIT_DBUS_IFACE,
                                           "StateChanged",
                                           // holy. nesting. hell.
                                           g_variant_new ("(o@a{sv})",
                                                          xdp_session_get_object_path (&inhibit_session->parent),
                                                          state),
                                           NULL);
    }
}

static void
on_logind_session_proxy_properties_changed (GDBusProxy  *proxy,
                                            GVariant    *changed_properties,
                                            GStrv        invalidated_properties,
                                            gpointer     user_data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (proxy);
  Inhibit *inhibit = user_data;
  InhibitSession *inhibit_session;
  g_autoptr(GVariant) state = NULL;
  GHashTableIter iter;
  g_autoptr(GVariant) locked_hint = NULL;

  if (!g_variant_lookup (changed_properties, "LockedHint", "v", locked_hint))
    return;

  state = g_variant_new ("a{sv}", g_variant_new("{sv}", "locked", locked_hint));

  g_hash_table_iter_init (&iter, inhibit->sessions);
  if (locked_hint != NULL)
    {
      while (g_hash_table_iter_next (&iter, (gpointer *) &inhibit_session, NULL))
          if (inhibit_session && !inhibit_session->closed)
            g_dbus_connection_emit_signal (connection,
                                           // is this the right argument here?
                                           inhibit_session->parent.sender,
                                           DESKTOP_DBUS_PATH,
                                           INHIBIT_DBUS_IFACE,
                                           "StateChanged",
                                           // holy. nesting. hell.
                                           g_variant_new ("(o@a{sv})",
                                                          xdp_session_get_object_path (&inhibit_session->parent),
                                                          state),
                                           NULL);
  }
}

static Inhibit *
inhibit_new (XdpContext         *context,
             XdpDbusImplInhibit *impl,
             GDBusProxy         *logind_proxy,
             GDBusProxy         *logind_session_proxy)
{
  Inhibit *inhibit;

  inhibit = g_object_new (inhibit_get_type (), NULL);
  inhibit->context = context;
  inhibit->impl = g_object_ref (impl);
  inhibit->last_session_state = STATE_RUNNING;
  inhibit->sessions = g_hash_table_new (g_direct_hash, g_direct_equal);
  // TODO: add suspend delay inhibitor

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
      g_warning ("Failed to create logind proxy for inhibit: %s",
                 error->message);

  sd_return = sd_pid_get_session (0, &logind_session_id);
  if (sd_return == -ENODATA)
    {
      sd_return = sd_uid_get_display (getuid(), &logind_session_id);
      if (sd_return < 0)
        g_warning ("Failed to get systemd session ID: %s",
                   strerror (sd_return));
    }

  if (logind_session_id != NULL)
    {
      if (g_ascii_isdigit (logind_session_id[0]))
        {
          logind_session_path =
            g_strdup_printf ("/org/freedesktop/login1/session/_3%s",
                             logind_session_id);
        }
      else
        {
          logind_session_path =
            g_strdup_printf ("/org/freedesktop/login1/session/%s",
                             logind_session_id);
        }

        logind_session_proxy =
          g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         NULL, LOGIN1_NAME,
                                         logind_session_path,
                                         LOGIN1_SESSION_INTERFACE,
                                         NULL, &error);
        if (logind_session_proxy == NULL)
            g_warning ("Failed to create logind session proxy for inhibit: %s",
                       error->message);
    }

  inhibit = inhibit_new (context, impl, logind_proxy, logind_session_proxy);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&inhibit)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);
}
