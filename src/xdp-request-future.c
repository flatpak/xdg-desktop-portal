/*
 * Copyright © 2025 Red Hat, Inc
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

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#include "xdp-request-future.h"

typedef struct _XdpRequestFuture
{
  XdpDbusRequestSkeleton parent_instance;

  XdpContext *context;
  XdpAppInfo *app_info;
  XdpDbusImplRequest *impl_request;
  GDBusInterfaceSkeleton *skeleton;
  char *id;
  gboolean exported;
} XdpRequestFuture;

static void xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpRequestFuture,
                         xdp_request_future,
                         XDP_DBUS_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REQUEST,
                                                xdp_request_skeleton_iface_init))

static void
xdp_request_future_on_signal_response (XdpDbusRequest *object,
                                       guint           arg_response,
                                       GVariant       *arg_results)
{
  XdpRequestFuture *request = XDP_REQUEST_FUTURE (object);

  g_dbus_connection_emit_signal (g_dbus_interface_skeleton_get_connection (request->skeleton),
                                 xdp_app_info_get_sender (request->app_info),
                                 request->id,
                                 DESKTOP_DBUS_IFACE ".Request",
                                 "Response",
                                 g_variant_new ("(u@a{sv})",
                                                arg_response,
                                                arg_results),
                                 NULL);
}

static gboolean
xdp_request_future_handle_close (XdpDbusRequest        *object,
                                 GDBusMethodInvocation *invocation)
{
  XdpRequestFuture *request = XDP_REQUEST_FUTURE (object);
  g_autoptr(GError) error = NULL;

  if (!request->exported)
    {
      xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  request->exported = FALSE;
  xdp_context_unclaim_object_path (request->context, request->id);

  dex_await (xdp_dbus_impl_request_call_close_future (request->impl_request),
             &error);
  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface)
{
  iface->handle_close = xdp_request_future_handle_close;
  iface->response = xdp_request_future_on_signal_response;
}

static void
xdp_request_future_dispose (GObject *object)
{
  XdpRequestFuture *request = XDP_REQUEST_FUTURE (object);

  if (request->exported)
    {
      xdp_dbus_impl_request_call_close (request->impl_request, NULL, NULL, NULL),

      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
      request->exported = FALSE;
      xdp_context_unclaim_object_path (request->context, request->id);
    }

  g_clear_object (&request->app_info);
  g_clear_object (&request->impl_request);
  g_clear_object (&request->skeleton);
  g_clear_pointer (&request->id, g_free);

  G_OBJECT_CLASS (xdp_request_future_parent_class)->dispose (object);
}

static void
xdp_request_future_init (XdpRequestFuture *request)
{
}

static void
xdp_request_future_class_init (XdpRequestFutureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_request_future_dispose;
}

static gboolean
request_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  XdpRequestFuture *request = XDP_REQUEST_FUTURE (user_data);
  const char *request_sender = xdp_app_info_get_sender (request->app_info);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_strcmp0 (sender, request_sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

typedef struct _RequestImplProxyCreateData {
  XdpContext *context;
  XdpAppInfo *app_info;
  GDBusInterfaceSkeleton *skeleton;
  char *id;
} RequestImplProxyCreateData;

static void
request_impl_proxy_create_data_free (RequestImplProxyCreateData *data)
{
  g_clear_object (&data->app_info);
  g_clear_object (&data->skeleton);
  g_clear_pointer (&data->id, g_free);
  free (data);
}

static void
on_peer_disconnect (XdpContext *context,
                    const char *peer,
                    gpointer    user_data)
{
  XdpRequestFuture *request = XDP_REQUEST_FUTURE (user_data);

  if (g_strcmp0 (xdp_app_info_get_sender (request->app_info), peer) != 0)
    return;

  if (!request->exported)
    return;

  xdp_dbus_impl_request_call_close (request->impl_request, NULL, NULL, NULL),

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  request->exported = FALSE;
  xdp_context_unclaim_object_path (request->context, request->id);
}

static DexFuture *
on_impl_request_proxy_created (DexFuture *future,
                               gpointer   user_data)
{
  RequestImplProxyCreateData *data = user_data;
  g_autoptr(XdpRequestFuture) request = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GError) error = NULL;

  impl_request = dex_await_object (dex_ref (future), NULL);
  g_assert (impl_request);

  request = g_object_new (XDP_TYPE_REQUEST_FUTURE, NULL);
  request->context = g_steal_pointer (&data->context);
  request->app_info = g_steal_pointer (&data->app_info);
  request->impl_request = g_steal_pointer (&impl_request);
  request->skeleton = g_steal_pointer (&data->skeleton);
  request->id = g_steal_pointer (&data->id);
  request->exported = TRUE;

  g_signal_connect_object (request->context, "peer-disconnect",
                           G_CALLBACK (on_peer_disconnect),
                           request,
                           G_CONNECT_DEFAULT);

  dex_dbus_interface_skeleton_set_flags (DEX_DBUS_INTERFACE_SKELETON (request),
                                         DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         g_dbus_interface_skeleton_get_connection (request->skeleton),
                                         request->id,
                                         &error))
      return dex_future_new_for_error (g_steal_pointer (&error));

  return dex_future_new_for_object (request);
}

DexFuture *
xdp_request_future_new (XdpContext             *context,
                        XdpAppInfo             *app_info,
                        GDBusInterfaceSkeleton *skeleton,
                        GDBusProxy             *proxy_impl,
                        GVariant               *arg_options)
{
  g_autoptr(DexFuture) future = NULL;
  RequestImplProxyCreateData *data;
  const char *token = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *id = NULL;

  g_variant_lookup (arg_options, "handle_token", "&s", &token);
  token = token ? token : "t";
  if (!xdp_is_valid_token (token))
    {
      return dex_future_new_for_error (g_error_new (XDG_DESKTOP_PORTAL_ERROR,
                                                    XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                    "Invalid token: %s", token));
    }

  sender = g_strdup (xdp_app_info_get_sender (app_info) + 1);
  for (size_t i = 0; sender[i]; i++)
    {
      if (sender[i] == '.')
        sender[i] = '_';
    }

  id = g_strdup_printf (DESKTOP_DBUS_PATH "/request/%s/%s", sender, token);

  while (!xdp_context_claim_object_path (context, id))
    {
      uint32_t r = g_random_int ();
      g_free (id);
      id = g_strdup_printf (DESKTOP_DBUS_PATH "/request/%s/%s/%u",
                            sender,
                            token,
                            r);
    }

  future = xdp_dbus_impl_request_proxy_new_future (
    g_dbus_proxy_get_connection (proxy_impl),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (proxy_impl),
    id);

  data = g_new0 (RequestImplProxyCreateData, 1);
  data->context = context;
  data->app_info = g_object_ref (app_info);
  data->skeleton = g_object_ref (skeleton);
  data->id = g_steal_pointer (&id);

  future = dex_future_then (future,
                            on_impl_request_proxy_created,
                            g_steal_pointer (&data),
                            (GDestroyNotify) request_impl_proxy_create_data_free);

  return g_steal_pointer (&future);
}

void
xdp_request_future_emit_response (XdpRequestFuture             *request,
                                  XdgDesktopPortalResponseEnum  response,
                                  GVariant                     *results)
{
  if (!request->exported)
    return;

  if (!results)
    {
      g_auto(GVariantBuilder) empty_results_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      results = g_variant_builder_end (&empty_results_builder);
    }

  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  response,
                                  results);
}

const char *
xdp_request_future_get_object_path (XdpRequestFuture *request)
{
  return request->id;
}
