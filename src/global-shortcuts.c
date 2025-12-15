/*
 * Copyright © 2022 Aleix Pol Gonzalez <aleixpol@kde.org>
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
 * Authors:
 *       Aleix Pol Gonzalez <aleixpol@kde.org>
 */

#include "config.h"

#include <string.h>
#include <glib-object.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-request-future.h"
#include "xdp-session-future.h"
#include "xdp-utils.h"

#include "global-shortcuts.h"

struct _XdpGlobalShortcuts
{
  XdpDbusGlobalShortcutsSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplGlobalShortcuts *impl;
  XdpSessionFutureStore *sessions;
};

#define XDP_TYPE_GLOBAL_SHORTCUTS (xdp_global_shortcuts_get_type ())
G_DECLARE_FINAL_TYPE (XdpGlobalShortcuts,
                      xdp_global_shortcuts,
                      XDP, GLOBAL_SHORTCUTS,
                      XdpDbusGlobalShortcutsSkeleton)

static void xdp_global_shortcuts_iface_init (XdpDbusGlobalShortcutsIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpGlobalShortcuts,
                               xdp_global_shortcuts,
                               XDP_DBUS_TYPE_GLOBAL_SHORTCUTS_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_GLOBAL_SHORTCUTS,
                                                      xdp_global_shortcuts_iface_init))

static XdpOptionKey xdp_global_shortcuts_keys[] = {
  { "description", G_VARIANT_TYPE_STRING, NULL },
  { "preferred_trigger", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
xdp_verify_shortcuts (GVariant         *shortcuts,
                      GVariantBuilder  *filtered,
                      GError          **error)
{
  char *shortcut_name;
  GVariant *values = NULL;
  g_autoptr(GVariantIter) iter = NULL;

  iter = g_variant_iter_new (shortcuts);
  while (g_variant_iter_loop (iter, "(s@a{sv})", &shortcut_name, &values))
    {
      g_auto(GVariantBuilder) shortcut_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      if (shortcut_name[0] == '\0')
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Unexpected empty shortcut id");
          return FALSE;
        }

      if (!xdp_filter_options (values,
                               &shortcut_builder,
                               xdp_global_shortcuts_keys,
                               G_N_ELEMENTS (xdp_global_shortcuts_keys),
                               NULL,
                               error))
        return FALSE;

      g_variant_builder_add (filtered, "(sa{sv})",
                             shortcut_name,
                             &shortcut_builder);
    }

  return TRUE;
}

static XdpOptionKey create_session_options[] = {
};

static gboolean
handle_create_session (XdpDbusGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       GVariant               *arg_options)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestFuture) request = NULL;
  g_autoptr(XdpSessionFuture) session = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  /* shortcuts really need to be scoped to a specific app */
  if (g_strcmp0 (xdp_app_info_get_id (app_info), "") == 0)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "An app id is required");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_auto(GVariantBuilder) options_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                             &options_builder,
                             create_session_options,
                             G_N_ELEMENTS (create_session_options),
                             NULL,
                             &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  }

  request = dex_await_object (xdp_request_future_new (global_shortcuts->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (global_shortcuts->impl),
                                                      arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = dex_await_object (xdp_session_future_new (global_shortcuts->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (global_shortcuts->impl),
                                                      arg_options),
                              &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_global_shortcuts_complete_create_session (object,
                                                     g_steal_pointer (&invocation),
                                                     xdp_request_future_get_object_path (request));

  {
    g_autoptr(XdpDbusImplGlobalShortcutsCreateSessionResult) result = NULL;
    XdgDesktopPortalResponseEnum response;
    g_auto(GVariantBuilder) results_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    result = dex_await_boxed (xdp_dbus_impl_global_shortcuts_call_create_session_future (
        global_shortcuts->impl,
        xdp_request_future_get_object_path (request),
        xdp_session_future_get_object_path (session),
        xdp_app_info_get_id (app_info),
        options),
      &error);

    if (result)
      {
        response = result->response;
        g_variant_builder_add (&results_builder, "{sv}",
                               "session_handle",
                               g_variant_new ("s",
                                              xdp_session_future_get_object_path (session)));

        xdp_session_future_store_take_session (global_shortcuts->sessions,
                                               g_steal_pointer (&session));
      }
    else
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Backend call failed: %s", error->message);

        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }

    xdp_request_future_emit_response (request,
                                      response,
                                      g_variant_builder_end (&results_builder));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey bind_shortcuts_options[] = {
};

static gboolean
handle_bind_shortcuts (XdpDbusGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       const char             *arg_session_handle,
                       GVariant               *arg_shortcuts,
                       const char             *arg_parent_window,
                       GVariant               *arg_options)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestFuture) request = NULL;
  XdpSessionFuture *session = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) shortcuts = NULL;
  g_autoptr(GError) error = NULL;

  {
    g_auto(GVariantBuilder) options_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                             &options_builder,
                             bind_shortcuts_options,
                             G_N_ELEMENTS (bind_shortcuts_options),
                             NULL,
                             &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  }

  {
    g_auto(GVariantBuilder) shortcuts_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(sa{sv})"));

    if (!xdp_verify_shortcuts (arg_shortcuts,
                               &shortcuts_builder,
                               &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    shortcuts = g_variant_ref_sink (g_variant_builder_end (&shortcuts_builder));
  }

  {
    session = xdp_session_future_store_lookup_session (global_shortcuts->sessions,
                                                       arg_session_handle,
                                                       app_info);
    if (!session)
      {
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                                G_DBUS_ERROR,
                                                G_DBUS_ERROR_ACCESS_DENIED,
                                                "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  {
    request = dex_await_object (xdp_request_future_new (global_shortcuts->context,
                                                        app_info,
                                                        G_DBUS_INTERFACE_SKELETON (object),
                                                        G_DBUS_PROXY (global_shortcuts->impl),
                                                        arg_options),
                                &error);
    if (!request)
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  xdp_dbus_global_shortcuts_complete_bind_shortcuts (object,
                                                     g_steal_pointer (&invocation),
                                                     xdp_request_future_get_object_path (request));

  {
    g_autoptr(XdpDbusImplGlobalShortcutsBindShortcutsResult) result = NULL;
    XdgDesktopPortalResponseEnum response;

    result = dex_await_boxed (xdp_dbus_impl_global_shortcuts_call_bind_shortcuts_future (
        global_shortcuts->impl,
        xdp_request_future_get_object_path (request),
        arg_session_handle,
        shortcuts,
        arg_parent_window,
        options),
      &error);

    if (result)
      {
        response = result->response;
      }
    else
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Backend call failed: %s", error->message);

        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }

    xdp_request_future_emit_response (request, response, NULL);
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey list_shortcuts_options[] = {
};

static gboolean
handle_list_shortcuts (XdpDbusGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       const gchar            *arg_session_handle,
                       GVariant               *arg_options)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestFuture) request = NULL;
  XdpSessionFuture *session = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  {
    g_auto(GVariantBuilder) options_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                             &options_builder,
                             list_shortcuts_options,
                             G_N_ELEMENTS (list_shortcuts_options),
                             NULL,
                             &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  }

  {
    session = xdp_session_future_store_lookup_session (global_shortcuts->sessions,
                                                       arg_session_handle,
                                                       app_info);
    if (!session)
      {
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                                G_DBUS_ERROR,
                                                G_DBUS_ERROR_ACCESS_DENIED,
                                                "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  {
    request = dex_await_object (xdp_request_future_new (global_shortcuts->context,
                                                        app_info,
                                                        G_DBUS_INTERFACE_SKELETON (object),
                                                        G_DBUS_PROXY (global_shortcuts->impl),
                                                        arg_options),
                                &error);
    if (!request)
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }


  xdp_dbus_global_shortcuts_complete_list_shortcuts (object,
                                                     g_steal_pointer (&invocation),
                                                     xdp_request_future_get_object_path (request));

  {
    g_autoptr(XdpDbusImplGlobalShortcutsListShortcutsResult) result = NULL;

    result = dex_await_boxed (xdp_dbus_impl_global_shortcuts_call_list_shortcuts_future (
        global_shortcuts->impl,
        xdp_request_future_get_object_path (request),
        arg_session_handle),
      &error);

    if (result)
      {
        xdp_request_future_emit_response (request,
                                          result->response,
                                          result->results);
      }
    else
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Backend call failed: %s", error->message);

        xdp_request_future_emit_response (request,
                                          XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                          NULL);
      }

  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey configure_shortcuts_options[] = {
  { "activation_token", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
handle_configure_shortcuts (XdpDbusGlobalShortcuts *object,
                            GDBusMethodInvocation  *invocation,
                            const char             *arg_session_handle,
                            const char             *arg_parent_window,
                            GVariant               *arg_options)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSessionFuture *session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  {
    g_auto(GVariantBuilder) options_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                             &options_builder,
                             configure_shortcuts_options,
                             G_N_ELEMENTS (configure_shortcuts_options),
                             NULL,
                             &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  }

  {
    session = xdp_session_future_store_lookup_session (global_shortcuts->sessions,
                                                       arg_session_handle,
                                                       app_info);
    if (!session)
      {
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                                G_DBUS_ERROR,
                                                G_DBUS_ERROR_ACCESS_DENIED,
                                                "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  {
    gboolean success;

    success = dex_await_boolean (xdp_dbus_impl_global_shortcuts_call_configure_shortcuts_future (
        global_shortcuts->impl,
        arg_session_handle,
        arg_parent_window,
        options),
      &error);

    if (!success)
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Failed to configure shortcuts: %s", error->message);
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    xdp_dbus_global_shortcuts_complete_configure_shortcuts (object,
                                                            g_steal_pointer (&invocation));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_global_shortcuts_iface_init (XdpDbusGlobalShortcutsIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_bind_shortcuts = handle_bind_shortcuts;
  iface->handle_list_shortcuts = handle_list_shortcuts;
  iface->handle_configure_shortcuts = handle_configure_shortcuts;
}

static void
xdp_global_shortcuts_dispose (GObject *object)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (object);

  g_clear_object (&global_shortcuts->impl);
  g_clear_object (&global_shortcuts->sessions);

  G_OBJECT_CLASS (xdp_global_shortcuts_parent_class)->dispose (object);
}

static void
xdp_global_shortcuts_init (XdpGlobalShortcuts *global_shortcuts)
{
}

static void
xdp_global_shortcuts_class_init (XdpGlobalShortcutsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_global_shortcuts_dispose;
}

static void
on_impl_activated (XdpDbusImplGlobalShortcuts *impl,
                   const char                 *session_id,
                   const char                 *shortcut_id,
                   guint64                     timestamp,
                   GVariant                   *options,
                   gpointer                    user_data)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (user_data);
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  XdpSessionFuture *session;
  XdpAppInfo *app_info;
  const char *sender;

  g_debug ("Received activated %s for %s", session_id, shortcut_id);

  session = xdp_session_future_store_lookup_session (global_shortcuts->sessions,
                                                     session_id,
                                                     NULL);

  if (!session || xdp_session_future_is_closed (session))
    return;

  app_info = xdp_session_future_get_app_info (session);
  sender = xdp_app_info_get_sender (app_info);
  g_dbus_connection_emit_signal (connection,
                                 sender,
                                 DESKTOP_DBUS_PATH,
                                 GLOBAL_SHORTCUTS_DBUS_IFACE,
                                 "Activated",
                                 g_variant_new ("(ost@a{sv})",
                                                session_id, shortcut_id,
                                                timestamp, options),
                                 NULL);
}

static void
on_impl_deactivated (XdpDbusImplGlobalShortcuts *impl,
                     const char                 *session_id,
                     const char                 *shortcut_id,
                     guint64                     timestamp,
                     GVariant                   *options,
                     gpointer                    user_data)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (user_data);
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  XdpSessionFuture *session;
  XdpAppInfo *app_info;
  const char *sender;

  g_debug ("Received deactivated %s for %s", session_id, shortcut_id);

  session = xdp_session_future_store_lookup_session (global_shortcuts->sessions,
                                                     session_id,
                                                     NULL);

  if (!session || xdp_session_future_is_closed (session))
    return;

  app_info = xdp_session_future_get_app_info (session);
  sender = xdp_app_info_get_sender (app_info);

  g_dbus_connection_emit_signal (connection,
                                 sender,
                                 DESKTOP_DBUS_PATH,
                                 GLOBAL_SHORTCUTS_DBUS_IFACE,
                                 "Deactivated",
                                 g_variant_new ("(ost@a{sv})",
                                                session_id, shortcut_id,
                                                timestamp, options),
                                 NULL);
}

static void
on_impl_shortcuts_changed (XdpDbusImplGlobalShortcuts *impl,
                           const char                 *session_id,
                           GVariant                   *shortcuts,
                           gpointer                    user_data)
{
  XdpGlobalShortcuts *global_shortcuts = XDP_GLOBAL_SHORTCUTS (user_data);
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  XdpSessionFuture *session;
  XdpAppInfo *app_info;
  const char *sender;

  g_debug ("Received ShortcutsChanged %s", session_id);

  session = xdp_session_future_store_lookup_session (global_shortcuts->sessions,
                                                     session_id,
                                                     NULL);

  if (!session || xdp_session_future_is_closed (session))
    return;

  app_info = xdp_session_future_get_app_info (session);
  sender = xdp_app_info_get_sender (app_info);

  g_dbus_connection_emit_signal (connection,
                                 sender,
                                 DESKTOP_DBUS_PATH,
                                 GLOBAL_SHORTCUTS_DBUS_IFACE,
                                 "ShortcutsChanged",
                                 g_variant_new ("(o@a(sa{sv}))", session_id, shortcuts),
                                 NULL);
}

static XdpGlobalShortcuts *
xdp_global_shortcuts_new (XdpContext                 *context,
                          XdpDbusImplGlobalShortcuts *impl)
{
  XdpGlobalShortcuts *global_shortcuts;

  global_shortcuts = g_object_new (XDP_TYPE_GLOBAL_SHORTCUTS, NULL);
  global_shortcuts->context = context;
  global_shortcuts->impl = g_object_ref (impl);
  global_shortcuts->sessions = xdp_session_future_store_new (0);

  g_signal_connect_object (global_shortcuts->impl, "activated",
                           G_CALLBACK (on_impl_activated),
                           global_shortcuts,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (global_shortcuts->impl, "deactivated",
                           G_CALLBACK (on_impl_deactivated),
                           global_shortcuts,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (global_shortcuts->impl, "shortcuts-changed",
                           G_CALLBACK (on_impl_shortcuts_changed),
                           global_shortcuts,
                           G_CONNECT_DEFAULT);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (global_shortcuts->impl),
                                    G_MAXINT);

  xdp_dbus_global_shortcuts_set_version (XDP_DBUS_GLOBAL_SHORTCUTS (global_shortcuts), 2);

  return global_shortcuts;
}

DexFuture *
init_global_shortcuts (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpGlobalShortcuts) global_shortcuts = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplGlobalShortcuts) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, GLOBAL_SHORTCUTS_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_global_shortcuts_proxy_new_future (
      connection,
      G_DBUS_PROXY_FLAGS_NONE,
      impl_config->dbus_name,
      "/org/freedesktop/portal/desktop"),
    &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create global_shortcuts proxy: %s", error->message);
      return dex_future_new_false ();
    }

  global_shortcuts = xdp_global_shortcuts_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&global_shortcuts)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
