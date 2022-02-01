/*
 * Copyright © 2016 Red Hat, Inc
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
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "inhibit.h"
#include "request.h"
#include "session.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "inhibit"
#define PERMISSION_ID "inhibit"

enum {
  INHIBIT_LOGOUT  = 1,
  INHIBIT_SWITCH  = 2,
  INHIBIT_SUSPEND = 4,
  INHIBIT_IDLE    = 8
};

#define INHIBIT_ALL (INHIBIT_LOGOUT|INHIBIT_SWITCH|INHIBIT_SUSPEND|INHIBIT_IDLE)

typedef struct _Inhibit Inhibit;
typedef struct _InhibitClass InhibitClass;

struct _Inhibit
{
  XdpInhibitSkeleton parent_instance;
};

struct _InhibitClass
{
  XdpInhibitSkeletonClass parent_class;
};

static XdpImplInhibit *impl;
static Inhibit *inhibit;

GType inhibit_get_type (void) G_GNUC_CONST;
static void inhibit_iface_init (XdpInhibitIface *iface);

G_DEFINE_TYPE_WITH_CODE (Inhibit, inhibit, XDP_TYPE_INHIBIT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_INHIBIT, inhibit_iface_init));

static void
inhibit_done (GObject *source,
              GAsyncResult *result,
              gpointer data)
{
  g_autoptr(GError) error = NULL;
  Request *request = data;
  int response = 0;

  REQUEST_AUTOLOCK (request);

  if (!xdp_impl_inhibit_call_inhibit_finish (impl, result, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      response = 2;
    }

  if (request->exported)
    {
      GVariantBuilder new_results;

      g_variant_builder_init (&new_results, G_VARIANT_TYPE_VARDICT);

      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&new_results));
    }
}

static guint32
get_allowed_inhibit (const char *app_id)
{
  g_auto(GStrv) perms = NULL;
  guint32 ret = 0;

  perms = get_permissions_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);

  if (perms != NULL)
    {
      int i;

      for (i = 0; perms[i]; i++)
        {
          if (strcmp (perms[i], "logout") == 0)
            ret |= INHIBIT_LOGOUT;
          else if (strcmp (perms[i], "switch") == 0)
            ret |= INHIBIT_SWITCH;
          else if (strcmp (perms[i], "suspend") == 0)
            ret |= INHIBIT_SUSPEND;
          else if (strcmp (perms[i], "idle") == 0)
            ret |= INHIBIT_IDLE;
          else
            g_warning ("Unknown inhibit flag in permission store: %s", perms[i]);
        }
    }
  else
    ret = INHIBIT_ALL; /* all allowed */

  g_debug ("Inhibit permissions for %s: %d", app_id, ret);

  return ret;
}

static void
handle_inhibit_in_thread_func (GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *window;
  guint32 flags;
  GVariant *options;
  const char *app_id;

  REQUEST_AUTOLOCK (request);

  window = (const char *)g_object_get_data (G_OBJECT (request), "window");
  flags = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (request), "flags"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  app_id = xdp_app_info_get_id (request->app_info);
  flags = flags & get_allowed_inhibit (app_id);

  if (flags == 0)
    return;

  g_debug ("Calling inhibit backend for %s: %d", app_id, flags);
  xdp_impl_inhibit_call_inhibit (impl,
                                 request->id,
                                 app_id,
                                 window,
                                 flags,
                                 options,
                                 NULL,
                                 inhibit_done,
                                 g_object_ref (request));
}

static gboolean
validate_reason (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
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
handle_inhibit (XdpInhibit *object,
                GDBusMethodInvocation *invocation,
                const char *arg_window,
                guint32 arg_flags,
                GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;
  GVariantBuilder opt_builder;
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

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &opt_builder,
                      inhibit_options, G_N_ELEMENTS (inhibit_options),
                      NULL);

  options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);
  g_object_set_data (G_OBJECT (request), "flags", GUINT_TO_POINTER (arg_flags));
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
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

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_inhibit_in_thread_func);

  xdp_inhibit_complete_inhibit (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

typedef struct _InhibitSession
{
  Session parent;

  gboolean closed;
} InhibitSession;

typedef struct _InhibitSessionClass
{
  SessionClass parent_class;
} InhibitSessionClass;

GType inhibit_session_get_type (void);

G_DEFINE_TYPE (InhibitSession, inhibit_session, session_get_type ())

static void
inhibit_session_close (Session *session)
{
  InhibitSession *inhibit_session = (InhibitSession *)session;

  inhibit_session->closed = TRUE;

  g_debug ("inhibit session owned by '%s' closed", session->sender);
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
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = inhibit_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = inhibit_session_close;
}

static InhibitSession *
inhibit_session_new (GVariant *options,
                     Request *request,
                     GError **error)
{
  Session *session;
  const char *session_token;
  GDBusInterfaceSkeleton *interface_skeleton = G_DBUS_INTERFACE_SKELETON (request);
  GDBusConnection *connection = g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (inhibit_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("inhibit session owned by '%s' created", session->sender);

  return (InhibitSession*)session;
}

static void
create_monitor_done (GObject *source_object,
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

  session = g_object_get_data (G_OBJECT (request), "session");
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_data (G_OBJECT (request), "session", NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_impl_inhibit_call_create_monitor_finish (impl, &response, res, &error))
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
handle_create_monitor (XdpInhibit *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_window,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  Session *session;

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

  session = (Session *)inhibit_session_new (arg_options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (request), "session", g_object_ref (session), g_object_unref);

  xdp_impl_inhibit_call_create_monitor (impl,
                                        request->id,
                                        session->id,
                                        xdp_app_info_get_id (request->app_info),
                                        arg_window,
                                        NULL,
                                        create_monitor_done,
                                        g_object_ref (request));

  xdp_inhibit_complete_create_monitor (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_query_end_response (XdpInhibit            *object,
                           GDBusMethodInvocation *invocation,
                           const char            *session_id)
{
  g_autoptr(Session) session = lookup_session (session_id);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_impl_inhibit_call_query_end_response (impl, session->id, NULL, NULL, NULL);
  xdp_inhibit_complete_query_end_response (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}


static void
inhibit_iface_init (XdpInhibitIface *iface)
{
  iface->handle_inhibit = handle_inhibit;
  iface->handle_create_monitor = handle_create_monitor;
  iface->handle_query_end_response = handle_query_end_response;
}

static void
inhibit_init (Inhibit *inhibit)
{
  xdp_inhibit_set_version (XDP_INHIBIT (inhibit), 3);
}

static void
inhibit_class_init (InhibitClass *klass)
{
}

static void
state_changed_cb (XdpImplInhibit *impl,
                  const char *session_id,
                  GVariant *state,
                  gpointer data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(Session) session = lookup_session (session_id);
  InhibitSession *inhibit_session = (InhibitSession *)session;
  gboolean active = FALSE;
  guint32 session_state = 0;

  g_variant_lookup (state, "screensaver-active", "b", &active);
  g_variant_lookup (state, "session-state", "u", &session_state);
  g_debug ("Received state-changed %s: screensaver-active: %d, session-state: %u",
           session_id, active, session_state);

  if (inhibit_session && !inhibit_session->closed)
    g_dbus_connection_emit_signal (connection,
                                   session->sender,
                                   "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.Inhibit",
                                   "StateChanged",
                                   g_variant_new ("(o@a{sv})", session_id, state),
                                   NULL);
}

GDBusInterfaceSkeleton *
inhibit_create (GDBusConnection *connection,
                const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_inhibit_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          dbus_name,
                                          "/org/freedesktop/portal/desktop",
                                          NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create inhibit proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  inhibit = g_object_new (inhibit_get_type (), NULL);

  g_signal_connect (impl, "state-changed", G_CALLBACK (state_changed_cb), inhibit);

  return G_DBUS_INTERFACE_SKELETON (inhibit);
}
