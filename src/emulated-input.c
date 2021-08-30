/*
 * Copyright © 2018-2019 Red Hat, Inc
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

#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixsocketaddress.h>
#include <stdio.h>

#include "session.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

/* The permission table is "emulated-input" : $appid */
#define PERMISSION_TABLE "emulated-input"

static XdpImplAccess *impl_access;
static XdpImplLockdown *lockdown;
static XdpImplEmulatedInput *impl;
static int impl_version;

typedef struct _EmulatedInput EmulatedInput;
typedef struct _EmulatedInputClass EmulatedInputClass;

static GQuark quark_request_session;

struct _EmulatedInput
{
  XdpEmulatedInputSkeleton parent_instance;
};

struct _EmulatedInputClass
{
  XdpEmulatedInputSkeletonClass parent_class;
};

static EmulatedInput *emulated_input;

GType emulated_input_get_type (void);
static void emulated_input_iface_init (XdpEmulatedInputIface *iface);

G_DEFINE_TYPE_WITH_CODE (EmulatedInput, emulated_input, XDP_TYPE_EMULATED_INPUT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_EMULATED_INPUT,
                                                emulated_input_iface_init))

typedef enum _EmulatedInputSessionState
{
  EMULATED_INPUT_SESSION_STATE_INIT,
  EMULATED_INPUT_SESSION_STATE_STARTING,
  EMULATED_INPUT_SESSION_STATE_STARTED,
  EMULATED_INPUT_SESSION_STATE_CLOSED
} EmulatedInputSessionState;

typedef struct _EmulatedInputSession
{
  Session parent;

  EmulatedInputSessionState state;
} EmulatedInputSession;

typedef struct _EmulatedInputSessionClass
{
  SessionClass parent_class;
} EmulatedInputSessionClass;

GType emulated_input_session_get_type (void);

G_DEFINE_TYPE (EmulatedInputSession, emulated_input_session, session_get_type ())

static gboolean
is_emulated_input_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, emulated_input_session_get_type ());
}

static EmulatedInputSession *
emulated_input_session_new (const char *session_token,
                            Request *request,
                            GError **error)
{
  Session *session;
  GDBusInterfaceSkeleton *interface_skeleton = G_DBUS_INTERFACE_SKELETON (request);
  GDBusConnection *connection =
    g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (impl));

  session = g_initable_new (emulated_input_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("emulated input session owned by '%s' created", session->sender);

  return (EmulatedInputSession*)session;
}

static void
create_session_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  guint response = 2;
  gboolean should_close_session = FALSE;
  GVariantBuilder results_builder;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_impl_emulated_input_call_create_session_finish (impl,
                                                           &response,
                                                           NULL,
                                                           res,
                                                           &error))
    {
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

  ((EmulatedInputSession*) session)->state = EMULATED_INPUT_SESSION_STATE_STARTED;

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
emulated_input_query_permission_sync (const char *app_id,
                                      Request    *request)
{
  Permission permission = PERMISSION_UNSET;
  gboolean allowed;

  /* If we don't have an app-id we can't check for anything */
  if (app_id == NULL || app_id[0] == '\0')
      return TRUE;

  if (permission == PERMISSION_ASK || permission == PERMISSION_UNSET)
    {
      GVariantBuilder opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GAppInfo) info = NULL;
      g_autoptr(XdpImplRequest) impl_request = NULL;

      if (app_id[0] != 0)
        {
          g_autofree char *desktop_id;
          desktop_id = g_strconcat (app_id, ".desktop", NULL);
          info = (GAppInfo*)g_desktop_app_info_new (desktop_id);
        }

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("input-mouse-symbolic"));

      title = g_strdup (_("Allow Emulated Input?"));
      body = g_strdup (_("Permissions to allow emulated input can be changed "
                         "at any time from the privacy settings."));

      if (info == NULL)
          subtitle = g_strdup (_("An application wants to emulate input."));
      else
          subtitle = g_strdup_printf (_("%s wants to emulate input."), g_app_info_get_display_name (info));

      impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl_access)),
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      g_dbus_proxy_get_name (G_DBUS_PROXY (impl_access)),
                                                      request->id,
                                                      NULL, &error);
      if (!impl_request)
        return FALSE;

      request_set_impl_request (request, impl_request);

      g_debug ("Calling backend for emulated input permission for: %s", app_id);

      if (!xdp_impl_access_call_access_dialog_sync (impl_access,
                                                    request->id,
                                                    app_id,
                                                    "",
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&opt_builder),
                                                    &response,
                                                    &results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("A backend call failed: %s", error ? error->message : "internal dbus error");
        }

      allowed = response == 0;

      if (permission == PERMISSION_UNSET)
        set_permission_sync (app_id, PERMISSION_TABLE, app_id, allowed ? PERMISSION_YES : PERMISSION_NO);
    }
  else
    allowed = permission == PERMISSION_YES ? TRUE : FALSE;

  g_debug ("Allowed? %d", allowed);
  return allowed;
}

static void
handle_create_session_in_thread_func (GTask *task,
                                      gpointer source_object,
                                      gpointer task_data,
                                      GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  Session *session = NULL;
  GVariantBuilder results;
  GVariantBuilder options;
  const char *app_id;
  const char *session_token;
  gboolean allowed;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;

  app_id = (const char *)g_object_get_data (G_OBJECT (request), "app-id");
  session_token = (const char *)g_object_get_data (G_OBJECT (request), "token");

  allowed = emulated_input_query_permission_sync (app_id, request);

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);

  if (!allowed) {
      response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      goto out;
  }

  impl_request =
    xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                     G_DBUS_PROXY_FLAGS_NONE,
                                     g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                     request->id,
                                     NULL, &error);
  if (!impl_request)
    {
      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      goto out;
    }

  request_set_impl_request (request, impl_request);

  session = (Session *)emulated_input_session_new (session_token, request, &error);
  if (!session)
    {
      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      goto out;
    }

  g_variant_builder_add (&results, "{sv}", "session_handle", g_variant_new ("s", session->id));

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_impl_emulated_input_call_create_session (impl,
                                               request->id,
                                               session->id,
                                               xdp_app_info_get_id (request->app_info),
                                               g_variant_builder_end (&options),
                                               NULL,
                                               create_session_done,
                                               g_object_ref (request));

  /* On success, the response is sent by create_session_done */
  return;

out:
  if (request->exported)
    {
      g_debug ("Emulated Input: sending response %d", response);
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_create_session (XdpEmulatedInput *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  const char *app_id;
  const char *session_token;

  REQUEST_AUTOLOCK (request);

  /* First, check for lockdown and return immediately */
  if (xdp_impl_lockdown_get_disable_emulated_input (lockdown))
    {
      g_debug ("Emulated Input access disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Emulated Input access disabled");
      return TRUE;
    }

  /* Store the appid in the request */
  app_id = xdp_app_info_get_id (request->app_info);
  g_object_set_data_full (G_OBJECT (request), "app-id", g_strdup (app_id), g_free);
  session_token = lookup_session_token (arg_options);
  g_object_set_data_full (G_OBJECT (request), "token", g_strdup (session_token), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_emulated_input_complete_create_session (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_create_session_in_thread_func);

  return TRUE;
}

static gboolean
handle_connect_to_eis (XdpEmulatedInput *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList *in_fd_list,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  EmulatedInputSession *emulated_input_session;
  g_autoptr(GError) error = NULL;
  GVariantBuilder unused;
  GVariantBuilder results_builder;
  GVariant *results = NULL;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  guint response = 1;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!is_emulated_input_session (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return TRUE;
    }

  emulated_input_session = (EmulatedInputSession *)session;
  switch (emulated_input_session->state)
    {
    case EMULATED_INPUT_SESSION_STATE_STARTED:
      break;
    case EMULATED_INPUT_SESSION_STATE_INIT:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session not started");
      return TRUE;
    case EMULATED_INPUT_SESSION_STATE_STARTING:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only start once");
      return TRUE;
    case EMULATED_INPUT_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return TRUE;
    }

  /* We don't have any options yet on the impl */
  g_variant_builder_init (&unused, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_impl_emulated_input_call_connect_to_eis_sync (impl,
                                                         arg_session_handle,
                                                         xdp_app_info_get_id (call->app_info),
                                                         g_variant_builder_end (&unused),
                                                         in_fd_list,
                                                         &response,
                                                         &results,
                                                         &out_fd_list,
                                                         NULL,
                                                         &error))
    {
      g_warning ("Failed to ConnectToEIS on impl: %s", error->message);
      out_fd_list = g_unix_fd_list_new();
    }

  if (response == 0)
    {
      gint out_fd;
      if (g_variant_lookup (results, "fd", "h", &out_fd))
        {
          g_variant_builder_add (&results_builder, "{sv}",
                                 "fd", g_variant_new("h", out_fd));
         response = 0;
        }
      else
        {
          g_error ("Key 'fd' missing in results");
        }
    }

  xdp_emulated_input_complete_connect_to_eis (object,
                                              invocation,
                                              out_fd_list,
                                              response,
                                              g_variant_builder_end(&results_builder));
  return TRUE;
}

static void
emulated_input_iface_init (XdpEmulatedInputIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_connect_to_eis = handle_connect_to_eis;
}

static void
emulated_input_finalize (GObject *object)
{
  G_OBJECT_CLASS (emulated_input_parent_class)->finalize (object);
}

static void
emulated_input_init (EmulatedInput *emulated_input)
{
  xdp_emulated_input_set_version (XDP_EMULATED_INPUT (emulated_input), 1);
}

static void
emulated_input_class_init (EmulatedInputClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = emulated_input_finalize;

  quark_request_session =
    g_quark_from_static_string ("-xdp-request-emulated-input-session");
}

GDBusInterfaceSkeleton *
emulated_input_create (GDBusConnection *connection,
                       const char *dbus_name,
                       gpointer lockdown_proxy)
{
  g_autoptr(GError) error = NULL;

  lockdown = lockdown_proxy;

  impl_access = xdp_impl_access_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                dbus_name,
                                                DESKTOP_PORTAL_OBJECT_PATH,
                                                NULL,
                                                &error);

  if (impl_access == NULL)
  {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
  }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl_access), G_MAXINT);

  impl = xdp_impl_emulated_input_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 dbus_name,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create emulated input proxy: %s", error->message);
      return NULL;
    }

  impl_version = xdp_impl_emulated_input_get_version (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  emulated_input = g_object_new (emulated_input_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (emulated_input);
}

static void
emulated_input_session_close (Session *session)
{
  EmulatedInputSession *emulated_input_session = (EmulatedInputSession *)session;

  emulated_input_session->state = EMULATED_INPUT_SESSION_STATE_CLOSED;

  g_debug ("emulated input session owned by '%s' closed", session->sender);
}

static void
emulated_input_session_finalize (GObject *object)
{
  G_OBJECT_CLASS (emulated_input_session_parent_class)->finalize (object);
}

static void
emulated_input_session_init (EmulatedInputSession *emulated_input_session)
{
}

static void
emulated_input_session_class_init (EmulatedInputSessionClass *klass)
{
  GObjectClass *object_class;
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = emulated_input_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = emulated_input_session_close;
}
