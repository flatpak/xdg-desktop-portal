/*
 * Copyright © 2022 Aleix Pol Gonzalez <aleixpol@kde.org>
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
 *       Aleix Pol Gonzalez <aleixpol@kde.org>
 */

#include "config.h"

#include <string.h>
#include <glib-object.h>

#include "global-shortcuts.h"
#include "request.h"
#include "session.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _GlobalShortcuts GlobalShortcuts;
typedef struct _GlobalShortcutsClass GlobalShortcutsClass;

static GQuark quark_request_session;

struct _GlobalShortcuts
{
  XdpDbusGlobalShortcutsSkeleton parent_instance;
};

struct _GlobalShortcutsClass
{
  XdpDbusGlobalShortcutsSkeletonClass parent_class;
};

static XdpDbusImplGlobalShortcuts *impl;
static GlobalShortcuts *global_shortcuts;

GType global_shortcuts_get_type (void) G_GNUC_CONST;
static void global_shortcuts_iface_init (XdpDbusGlobalShortcutsIface *iface);

G_DEFINE_TYPE_WITH_CODE (GlobalShortcuts, global_shortcuts, XDP_DBUS_TYPE_GLOBAL_SHORTCUTS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_GLOBAL_SHORTCUTS, global_shortcuts_iface_init));

typedef struct _GlobalShortcutsSession
{
  Session parent;

  gboolean closed;
} GlobalShortcutsSession;

typedef struct _GlobalShortcutsSessionClass
{
  SessionClass parent_class;
} GlobalShortcutsSessionClass;

GType global_shortcuts_session_get_type (void);

G_DEFINE_TYPE (GlobalShortcutsSession, global_shortcuts_session, session_get_type ())

static void
global_shortcuts_session_close (Session *session)
{
  GlobalShortcutsSession *global_shortcuts_session = (GlobalShortcutsSession *)session;

  global_shortcuts_session->closed = TRUE;
}

static void
global_shortcuts_session_finalize (GObject *object)
{
  G_OBJECT_CLASS (global_shortcuts_session_parent_class)->finalize (object);
}

static void
global_shortcuts_session_init (GlobalShortcutsSession *global_shortcuts_session)
{
}

static void
global_shortcuts_session_class_init (GlobalShortcutsSessionClass *klass)
{
  GObjectClass *object_class;
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = global_shortcuts_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = global_shortcuts_session_close;
}

static GlobalShortcutsSession *
global_shortcuts_session_new (GVariant *options,
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
  session = g_initable_new (global_shortcuts_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    g_debug ("global shortcuts session owned by '%s' created", session->sender);

  return (GlobalShortcutsSession *) session;
}

static void
session_created_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  guint response = 2;
  g_autoptr (GVariant) results = NULL;
  gboolean should_close_session;
  GVariantBuilder results_builder;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_dbus_impl_global_shortcuts_call_create_session_finish (impl,
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

static XdpOptionKey global_shortcuts_create_session_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "session_handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
handle_create_session (XdpDbusGlobalShortcuts *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariantBuilder options_builder;
  g_autoptr(GVariant) options = NULL;
  Session *session;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           global_shortcuts_create_session_options,
                           G_N_ELEMENTS (global_shortcuts_create_session_options),
                           &error))
    {
      g_variant_builder_clear(&options_builder);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);
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

  session = (Session *)global_shortcuts_session_new (options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_global_shortcuts_call_create_session (impl,
                                                      request->id,
                                                      session->id,
                                                      xdp_app_info_get_id (request->app_info),
                                                      g_steal_pointer (&options),
                                                      NULL,
                                                      session_created_cb,
                                                      g_object_ref (request));

  xdp_dbus_global_shortcuts_complete_create_session (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

void
shortcuts_bound_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  guint response = 2;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) results = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_dbus_impl_global_shortcuts_call_bind_shortcuts_finish (impl, &response, &results, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  if (request->exported)
    {
      if (!results)
        {
          GVariantBuilder results_builder;

          g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      request_unexport (request);
    }
}

static XdpOptionKey global_shortcuts_keys[] = {
  { "description", G_VARIANT_TYPE_STRING, NULL },
  { "preferred_trigger", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
xdp_verify_shortcuts (GVariant *shortcuts,
                      GVariantBuilder *filtered,
                      GError **error)
{
  gchar *shortcut_name;
  GVariant *values = NULL;
  g_autoptr(GVariantIter) iter = NULL;

  iter = g_variant_iter_new (shortcuts);
  while (g_variant_iter_loop (iter, "(s@a{sv})", &shortcut_name, &values))
    {
      GVariantBuilder shortcut_builder;

      if (shortcut_name[0] == 0)
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Unexpected empty shortcut id");
          return FALSE;
        }

      g_variant_builder_init (&shortcut_builder, G_VARIANT_TYPE_VARDICT);
      if (!xdp_filter_options (values, &shortcut_builder,
                               global_shortcuts_keys,
                               G_N_ELEMENTS (global_shortcuts_keys),
                               error))
        return FALSE;
      g_variant_builder_add (filtered, "(sa{sv})",
                             shortcut_name,
                             &shortcut_builder);
    }
  return TRUE;
}

static XdpOptionKey global_shortcuts_bind_shortcuts_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
handle_bind_shortcuts (XdpDbusGlobalShortcuts *object,
                       GDBusMethodInvocation *invocation,
                       const gchar *arg_session_handle,
                       GVariant *arg_shortcuts,
                       const gchar *arg_parent_window,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) shortcuts = NULL;
  GVariantBuilder shortcuts_builder;
  GVariantBuilder options_builder;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           global_shortcuts_bind_shortcuts_options,
                           G_N_ELEMENTS (global_shortcuts_bind_shortcuts_options),
                           &error))
    {
      g_variant_builder_clear (&options_builder);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  g_variant_builder_init (&shortcuts_builder, G_VARIANT_TYPE_ARRAY);
  if (!xdp_verify_shortcuts (arg_shortcuts, &shortcuts_builder,
                             &error))
    {
      g_variant_builder_clear (&shortcuts_builder);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  shortcuts = g_variant_builder_end (&shortcuts_builder);

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

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_global_shortcuts_call_bind_shortcuts (impl,
                                                      request->id,
                                                      arg_session_handle,
                                                      g_steal_pointer (&shortcuts),
                                                      arg_parent_window,
                                                      g_steal_pointer (&options),
                                                      NULL,
                                                      shortcuts_bound_cb,
                                                      g_object_ref (request));

  xdp_dbus_global_shortcuts_complete_bind_shortcuts (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
shortcuts_listed_cb (GObject *source_object,
                     GAsyncResult *res,
                     gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  guint response = 2;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) results = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  if (!xdp_dbus_impl_global_shortcuts_call_list_shortcuts_finish (impl, &response, &results, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  if (request->exported)
    {
      if (!results)
        {
          GVariantBuilder results_builder;

          g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
          results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
        }

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request), response, results);
      request_unexport (request);
    }
}

static XdpOptionKey global_shortcuts_list_shortcuts_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
handle_list_shortcuts (XdpDbusGlobalShortcuts *object,
                       GDBusMethodInvocation *invocation,
                       const gchar *arg_session_handle,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder options_builder;
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           global_shortcuts_list_shortcuts_options,
                           G_N_ELEMENTS (global_shortcuts_list_shortcuts_options),
                           &error))
    {
      g_variant_builder_clear (&options_builder);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_builder_end (&options_builder);

  session = acquire_session (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
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

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  xdp_dbus_impl_global_shortcuts_call_list_shortcuts (impl,
                                                      request->id,
                                                      arg_session_handle,
                                                      NULL,
                                                      shortcuts_listed_cb,
                                                      g_object_ref (request));

  xdp_dbus_global_shortcuts_complete_list_shortcuts (object, invocation, request->id);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
global_shortcuts_iface_init (XdpDbusGlobalShortcutsIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_bind_shortcuts = handle_bind_shortcuts;
  iface->handle_list_shortcuts = handle_list_shortcuts;
}

static void
global_shortcuts_init (GlobalShortcuts *global_shortcuts)
{
  xdp_dbus_global_shortcuts_set_version (XDP_DBUS_GLOBAL_SHORTCUTS (global_shortcuts), 1);
}

static void
global_shortcuts_class_init (GlobalShortcutsClass *klass)
{
  quark_request_session =
    g_quark_from_static_string ("-xdp-request-global-shortcuts-session");
}

static void
activated_cb (XdpDbusImplGlobalShortcuts *impl,
              const char *session_id,
              const char *shortcut_id,
              guint64 timestamp,
              GVariant *options,
              gpointer data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(Session) session = lookup_session (session_id);
  GlobalShortcutsSession *global_shortcuts_session = (GlobalShortcutsSession *)session;

  g_debug ("Received activated %s for %s", session_id, shortcut_id);

  if (global_shortcuts_session && !global_shortcuts_session->closed)
    g_dbus_connection_emit_signal (connection,
                                   session->sender,
                                   "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.GlobalShortcuts",
                                   "Activated",
                                   g_variant_new ("(ost@a{sv})",
                                                  session_id, shortcut_id,
                                                  timestamp, options),
                                   NULL);
}

static void
deactivated_cb (XdpDbusImplGlobalShortcuts *impl,
                const char *session_id,
                const char *shortcut_id,
                guint64 timestamp,
                GVariant *options,
                gpointer data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(Session) session = lookup_session (session_id);
  GlobalShortcutsSession *global_shortcuts_session = (GlobalShortcutsSession *)session;

  g_debug ("Received deactivated %s for %s", session_id, shortcut_id);

  if (global_shortcuts_session && !global_shortcuts_session->closed)
    g_dbus_connection_emit_signal (connection,
                                   session->sender,
                                   "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.GlobalShortcuts",
                                   "Deactivated",
                                   g_variant_new ("(ost@a{sv})",
                                                  session_id, shortcut_id,
                                                  timestamp, options),
                                   NULL);
}

static void
shortcuts_changed_cb (XdpDbusImplGlobalShortcuts *impl,
                      const char *session_id,
                      GVariant *shortcuts,
                      gpointer data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  g_autoptr(Session) session = lookup_session (session_id);
  GlobalShortcutsSession *global_shortcuts_session = (GlobalShortcutsSession *)session;

  g_debug ("Received ShortcutsChanged %s", session_id);

  if (global_shortcuts_session && !global_shortcuts_session->closed)
    g_dbus_connection_emit_signal (connection,
                                   session->sender,
                                   "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.GlobalShortcuts",
                                   "ShortcutsChanged",
                                   g_variant_new ("(o@a(sa{sv}))", session_id, shortcuts),
                                   NULL);
}

GDBusInterfaceSkeleton *
global_shortcuts_create (GDBusConnection *connection,
                         const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_global_shortcuts_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        dbus_name,
                                                        "/org/freedesktop/portal/desktop",
                                                        NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create global_shortcuts proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);
  global_shortcuts = g_object_new (global_shortcuts_get_type (), NULL);

  g_signal_connect (impl, "activated", G_CALLBACK (activated_cb), global_shortcuts);
  g_signal_connect (impl, "deactivated", G_CALLBACK (deactivated_cb), global_shortcuts);
  g_signal_connect (impl, "shortcuts-changed", G_CALLBACK (shortcuts_changed_cb), global_shortcuts);

  return G_DBUS_INTERFACE_SKELETON (global_shortcuts);
}
