/*
 * Copyright © 2016 Red Hat, Inc
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
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-queue-future.h"
#include "xdp-request-future.h"
#include "xdp-session-future.h"
#include "xdp-utils.h"

#include "inhibit.h"

enum {
  INHIBIT_LOGOUT       = (1 << 0),
  INHIBIT_USER_SWITCH  = (1 << 1),
  INHIBIT_SUSPEND      = (1 << 2),
  INHIBIT_IDLE         = (1 << 3),
  INHIBIT_ALL          = (1 << 4) - 1,
};

struct _XdpInhibit
{
  XdpDbusInhibitSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplInhibit *impl;
  XdpSessionFutureStore *sessions;
};

#define XDP_TYPE_INHIBIT (xdp_inhibit_get_type ())
G_DECLARE_FINAL_TYPE (XdpInhibit,
                      xdp_inhibit,
                      XDP, INHIBIT,
                      XdpDbusInhibitSkeleton)

static void xdp_inhibit_iface_init (XdpDbusInhibitIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpInhibit,
                               xdp_inhibit,
                               XDP_DBUS_TYPE_INHIBIT_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_INHIBIT,
                                                      xdp_inhibit_iface_init));

/* Ideally, we would scope the order per session, but the Inhibit method
 * can be called without a session, so we scope it to the peer instead. */
static XdpQueueFuture *
ensure_app_info_queue_future (XdpAppInfo *app_info)
{
  XdpQueueFuture *queue_future;

  queue_future = g_object_get_data (G_OBJECT (app_info),
                                    "-xdp-inhibit-queue-future");

  if (!queue_future)
    {
      queue_future = xdp_queue_future_new ();
      g_object_set_data_full (G_OBJECT (app_info),
                              "-xdp-inhibit-queue-future",
                              queue_future,
                              g_object_unref);
    }

  return queue_future;
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
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "Not accepting overly long reasons");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey inhibit_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason }
};

static gboolean
handle_inhibit (XdpDbusInhibit        *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_window,
                uint32_t               arg_flags,
                GVariant              *arg_options)
{
  XdpInhibit *inhibit = XDP_INHIBIT (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestFuture) request = NULL;
  g_autoptr(GVariant) options = NULL;
  uint32_t flags;
  g_autoptr(XdpQueueFutureGuard) guard = NULL;
  g_autoptr(GError) error = NULL;

  guard = dex_await_object (xdp_queue_future_next (ensure_app_info_queue_future (app_info)), NULL);
  g_assert (guard);

  {
    g_auto(GVariantBuilder) options_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                             &options_builder,
                             inhibit_options,
                             G_N_ELEMENTS (inhibit_options),
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
    g_auto(GStrv) perms = NULL;
    uint32_t allowed_flags = 0;

    if ((arg_flags & ~INHIBIT_ALL) != 0)
      {
        g_dbus_method_invocation_return_error_literal (g_steal_pointer (&invocation),
                                                       XDG_DESKTOP_PORTAL_ERROR,
                                                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                       "Invalid flags");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    perms = dex_await_pointer (xdp_permissions_get_future (app_info,
                                                           INHIBIT_PERMISSION_TABLE,
                                                           INHIBIT_PERMISSION_ID),
                               NULL);

    if (perms == NULL)
      allowed_flags = INHIBIT_ALL; /* all allowed */

    for (size_t i = 0; perms && perms[i]; i++)
      {
        if (strcmp (perms[i], "logout") == 0)
          allowed_flags |= INHIBIT_LOGOUT;
        else if (strcmp (perms[i], "switch") == 0)
          allowed_flags |= INHIBIT_USER_SWITCH;
        else if (strcmp (perms[i], "suspend") == 0)
          allowed_flags |= INHIBIT_SUSPEND;
        else if (strcmp (perms[i], "idle") == 0)
          allowed_flags |= INHIBIT_IDLE;
        else
          g_warning ("Unknown inhibit flag in permission store: %s", perms[i]);
      }

    g_debug ("Inhibit permissions for %s: %d",
             xdp_app_info_get_id (app_info),
             allowed_flags);

    flags = arg_flags & allowed_flags;
  }

  request = dex_await_object (xdp_request_future_new (inhibit->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (inhibit->impl),
                                                      arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_inhibit_complete_inhibit (object,
                                     g_steal_pointer (&invocation),
                                     xdp_request_future_get_object_path (request));

  if (flags == 0)
    {
      xdp_request_future_emit_response (request,
                                        XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                        NULL);
    }
  else
    {
      gboolean success;
      XdgDesktopPortalResponseEnum response;

      success = dex_await_boolean (xdp_dbus_impl_inhibit_call_inhibit_future (
          inhibit->impl,
          xdp_request_future_get_object_path (request),
          xdp_app_info_get_id (app_info),
          arg_window,
          flags,
          options),
        &error);

      if (success)
        {
          response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
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

static gboolean
handle_create_monitor (XdpDbusInhibit        *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_window,
                       GVariant              *arg_options)
{
  XdpInhibit *inhibit = XDP_INHIBIT (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpRequestFuture) request = NULL;
  g_autoptr(XdpSessionFuture) session = NULL;
  g_autoptr(GError) error = NULL;

  request = dex_await_object (xdp_request_future_new (inhibit->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (inhibit->impl),
                                                      arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = dex_await_object (xdp_session_future_new (inhibit->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (inhibit->impl),
                                                      arg_options),
                              &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_inhibit_complete_create_monitor (object,
                                            g_steal_pointer (&invocation),
                                            xdp_request_future_get_object_path (request));

  {
    g_autoptr(XdpDbusImplInhibitCreateMonitorResult) result = NULL;
    XdgDesktopPortalResponseEnum response;
    g_auto(GVariantBuilder) results_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    result = dex_await_boxed (xdp_dbus_impl_inhibit_call_create_monitor_future (
        inhibit->impl,
        xdp_request_future_get_object_path (request),
        xdp_session_future_get_object_path (session),
        xdp_app_info_get_id (app_info),
        arg_window),
      &error);

    if (result)
      {
        response = result->response;
        g_variant_builder_add (&results_builder, "{sv}",
                               "session_handle",
                               g_variant_new ("s",
                                              xdp_session_future_get_object_path (session)));

        xdp_session_future_store_take_session (inhibit->sessions,
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

static gboolean
handle_query_end_response (XdpDbusInhibit        *object,
                           GDBusMethodInvocation *invocation,
                           const char            *arg_session_handle)
{
  XdpInhibit *inhibit = XDP_INHIBIT (object);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSessionFuture *session;
  g_autoptr(XdpQueueFutureGuard) guard = NULL;
  g_autoptr(GError) error = NULL;

  guard = dex_await_object (xdp_queue_future_next (ensure_app_info_queue_future (app_info)), NULL);
  g_assert (guard);

  session = xdp_session_future_store_lookup_session (inhibit->sessions,
                                                     arg_session_handle,
                                                     app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error_literal (g_steal_pointer (&invocation),
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_ACCESS_DENIED,
                                                     "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  dex_await (xdp_dbus_impl_inhibit_call_query_end_response_future (
      inhibit->impl,
      xdp_session_future_get_object_path (session)),
    &error);

  if (error != NULL)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_inhibit_complete_query_end_response (object,
                                                g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_inhibit_iface_init (XdpDbusInhibitIface *iface)
{
  iface->handle_inhibit = handle_inhibit;
  iface->handle_create_monitor = handle_create_monitor;
  iface->handle_query_end_response = handle_query_end_response;
}

static void
xdp_inhibit_dispose (GObject *object)
{
  XdpInhibit *inhibit = XDP_INHIBIT (object);

  g_clear_object (&inhibit->impl);
  g_clear_object (&inhibit->sessions);

  G_OBJECT_CLASS (xdp_inhibit_parent_class)->dispose (object);
}

static void
xdp_inhibit_init (XdpInhibit *inhibit)
{
}

static void
xdp_inhibit_class_init (XdpInhibitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_inhibit_dispose;
}

static void
on_state_changed (XdpDbusImplInhibit *impl,
                  const char         *session_id,
                  GVariant           *state,
                  gpointer            user_data)
{
  XdpInhibit *inhibit = XDP_INHIBIT (user_data);
  GDBusConnection *connection;
  XdpAppInfo *app_info;
  XdpSessionFuture *session;
  gboolean active = FALSE;
  uint32_t session_state = 0;

  g_variant_lookup (state, "screensaver-active", "b", &active);
  g_variant_lookup (state, "session-state", "u", &session_state);

  g_debug ("Received state-changed %s: screensaver-active: %d, session-state: %u",
           session_id, active, session_state);

  session = xdp_session_future_store_lookup_session (inhibit->sessions,
                                                     session_id,
                                                     NULL);
  if (!session)
    return;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (inhibit->impl));
  app_info = xdp_session_future_get_app_info (session);

  g_dbus_connection_emit_signal (connection,
                                 xdp_app_info_get_sender (app_info),
                                 DESKTOP_DBUS_PATH,
                                 INHIBIT_DBUS_IFACE,
                                 "StateChanged",
                                 g_variant_new ("(o@a{sv})", session_id, state),
                                 NULL);
}

static XdpInhibit *
xdp_inhibit_new (XdpContext         *context,
                 XdpDbusImplInhibit *impl)
{
  XdpInhibit *inhibit;

  inhibit = g_object_new (XDP_TYPE_INHIBIT, NULL);
  inhibit->context = context;
  inhibit->impl = g_object_ref (impl);
  inhibit->sessions = xdp_session_future_store_new (0);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (inhibit->impl), G_MAXINT);

  g_signal_connect_object (inhibit->impl, "state-changed",
                           G_CALLBACK (on_state_changed),
                           inhibit,
                           G_CONNECT_DEFAULT);

  xdp_dbus_inhibit_set_version (XDP_DBUS_INHIBIT (inhibit), 3);

  return inhibit;
}

DexFuture *
init_inhibit (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpInhibit) inhibit = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplInhibit) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, INHIBIT_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_inhibit_proxy_new_future (connection,
                                                                   G_DBUS_PROXY_FLAGS_NONE,
                                                                   impl_config->dbus_name,
                                                                   DESKTOP_DBUS_PATH),
                           &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create inhibit proxy: %s", error->message);
      return dex_future_new_false ();
    }

  inhibit = xdp_inhibit_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&inhibit)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  return dex_future_new_true ();
}
