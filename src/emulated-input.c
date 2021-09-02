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

#include "session.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

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
  EMULATED_INPUT_SESSION_STATE_NEW,
  EMULATED_INPUT_SESSION_STATE_CONNECTING,
  EMULATED_INPUT_SESSION_STATE_CONNECTED,
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
handle_create_session (XdpEmulatedInput *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  Session *session;
  GVariantBuilder options_builder;
  GVariant *options;
  const char *session_token;

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
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session_token = lookup_session_token (arg_options);
  session = (Session *)emulated_input_session_new (session_token, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&options_builder);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_impl_emulated_input_call_create_session (impl,
                                               request->id,
                                               session->id,
                                               xdp_app_info_get_id (request->app_info),
                                               options,
                                               NULL,
                                               create_session_done,
                                               g_object_ref (request));

  xdp_emulated_input_complete_create_session (object, invocation, request->id);

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
    case EMULATED_INPUT_SESSION_STATE_NEW:
      break;
    case EMULATED_INPUT_SESSION_STATE_CONNECTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session already connected");
      return TRUE;
    case EMULATED_INPUT_SESSION_STATE_CONNECTING:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only connect once");
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

  emulated_input_session->state = EMULATED_INPUT_SESSION_STATE_CONNECTING;

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
         emulated_input_session->state = EMULATED_INPUT_SESSION_STATE_CONNECTED;
        }
      else
        {
          g_error ("Key 'fd' missing in results");
          response = 1;
        }
    }

  if (response != 0)
    {
        emulated_input_session->state = EMULATED_INPUT_SESSION_STATE_CLOSED;
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
