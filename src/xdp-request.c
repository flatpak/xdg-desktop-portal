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
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "xdp-request.h"
#include "xdp-utils.h"
#include "xdp-method-info.h"

#include <string.h>

static void xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpRequest, xdp_request, XDP_DBUS_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REQUEST,
                                                xdp_request_skeleton_iface_init))

static void
xdp_request_on_signal_response (XdpDbusRequest *object,
                                guint           arg_response,
                                GVariant       *arg_results)
{
  XdpRequest *request = XDP_REQUEST (object);
  XdpDbusRequestSkeleton *skeleton = XDP_DBUS_REQUEST_SKELETON (object);
  GList      *connections, *l;
  GVariant   *signal_variant;

  connections = g_dbus_interface_skeleton_get_connections (G_DBUS_INTERFACE_SKELETON (skeleton));

  signal_variant = g_variant_ref_sink (g_variant_new ("(u@a{sv})",
                                                      arg_response,
                                                      arg_results));
  for (l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     request->sender,
                                     g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)),
                                     "org.freedesktop.portal.Request",
                                     "Response",
                                     signal_variant,
                                     NULL);
    }
  g_variant_unref (signal_variant);
  g_list_free_full (connections, g_object_unref);
}

static gboolean
xd_request_handle_close (XdpDbusRequest        *object,
                         GDBusMethodInvocation *invocation)
{
  XdpRequest *request = XDP_REQUEST (object);
  g_autoptr(GError) error = NULL;

  g_debug ("Handling Close");
  REQUEST_AUTOLOCK (request);

  if (request->exported)
    {
      if (request->impl_request &&
          !xdp_dbus_impl_request_call_close_sync (request->impl_request,
                                                  NULL, &error))
        {
          if (invocation)
            g_dbus_method_invocation_return_gerror (invocation, error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      xdp_request_unexport (request);
    }

  if (invocation)
    xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_request_skeleton_iface_init (XdpDbusRequestIface *iface)
{
  iface->handle_close = xd_request_handle_close;
  iface->response = xdp_request_on_signal_response;
}

G_LOCK_DEFINE (requests);
static GHashTable *requests;

static void
xdp_request_init (XdpRequest *request)
{
  g_mutex_init (&request->mutex);
}

static void
xdp_request_finalize (GObject *object)
{
  XdpRequest *request = XDP_REQUEST (object);

  G_LOCK (requests);
  g_hash_table_remove (requests, request->id);
  G_UNLOCK (requests);

  g_clear_object (&request->impl_request);
  g_clear_pointer (&request->sender, g_free);
  g_clear_pointer (&request->id, g_free);
  g_mutex_clear (&request->mutex);
  g_clear_object (&request->app_info);

  G_OBJECT_CLASS (xdp_request_parent_class)->finalize (object);
}

static void
xdp_request_class_init (XdpRequestClass *klass)
{
  GObjectClass *gobject_class;

  requests = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize  = xdp_request_finalize;
}

static gboolean
request_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  const gchar *request_sender = user_data;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (strcmp (sender, request_sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

/* This is a bit ugly - we need to know where the options vardict is
 * in the parameters for each request. Instead of inventing some
 * complicated mechanism for each implementation to provide that
 * information, just hardcode it here for now.
 *
 * Note that the pointer returned by this function is good to use
 * as long as the invocation object exists, since it points at data
 * in the parameters variant.
 */
static const char *
get_token (GDBusMethodInvocation *invocation)
{
  const char *interface;
  const char *method;
  GVariant *parameters;
  g_autoptr(GVariant) options = NULL;
  const char *token = NULL;
  const XdpMethodInfo *method_info;

  interface = g_dbus_method_invocation_get_interface_name (invocation);
  method = g_dbus_method_invocation_get_method_name (invocation);
  parameters = g_dbus_method_invocation_get_parameters (invocation);

  method_info = xdp_method_info_find (interface, method);
  if (method_info)
    {
      if (method_info->option_arg >= 0)
        options = g_variant_get_child_value (parameters, method_info->option_arg);
    }
  else
    {
      g_warning ("Support for %s::%s missing in %s",
                 interface, method, G_STRLOC);
    }

  if (options)
    g_variant_lookup (options, "handle_token", "&s", &token);

  return token ? token : "t";
}

gboolean
xdp_request_init_invocation (GDBusMethodInvocation *invocation,
                             XdpAppInfo            *app_info,
                             GError               **error)
{
  XdpRequest *request;
  guint32 r;
  char *id = NULL;
  const char *token;
  g_autofree char *sender = NULL;
  int i;

  token = get_token (invocation);
  if (!xdp_is_valid_token (token))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid token: %s", token);
      return FALSE;
    }

  request = g_object_new (xdp_request_get_type (), NULL);
  request->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  request->app_info = g_object_ref (app_info);

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (-1));

  sender = g_strdup (request->sender + 1);
  for (i = 0; sender[i]; i++)
    if (sender[i] == '.')
      sender[i] = '_';

  id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

  G_LOCK (requests);

  while (g_hash_table_lookup (requests, id) != NULL)
    {
      r = g_random_int ();
      g_free (id);
      id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s/%u", sender, token, r);
    }

  request->id = id;
  g_hash_table_insert (requests, id, request);

  G_UNLOCK (requests);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (request),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request->sender);


  g_object_set_data_full (G_OBJECT (invocation), "request", request, g_object_unref);
  return TRUE;
}

XdpRequest *
xdp_request_from_invocation (GDBusMethodInvocation *invocation)
{
  return g_object_get_data (G_OBJECT (invocation), "request");
}

const char *
xdp_request_get_object_path (XdpRequest *request)
{
  return request->id;
}

void
xdp_request_export (XdpRequest      *request,
                    GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         connection,
                                         request->id,
                                         &error))
    {
      g_warning ("Error exporting request: %s", error->message);
      g_clear_error (&error);
    }

  g_object_ref (request);
  request->exported = TRUE;
}

void
xdp_request_unexport (XdpRequest *request)
{
  int fd;

  fd = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "fd"));
  if (fd != -1)
    close (fd);

  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}

void
xdp_request_set_impl_request (XdpRequest         *request,
                              XdpDbusImplRequest *impl_request)
{
  g_set_object (&request->impl_request, impl_request);
}

static void
xdp_close_requests_in_thread_func (GTask        *task,
                                   gpointer      source_object,
                                   gpointer      task_data,
                                   GCancellable *cancellable)
{
  const char *sender = (const char *)task_data;
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  XdpRequest *request;

  G_LOCK (requests);
  if (requests)
    {
      g_hash_table_iter_init (&iter, requests);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&request))
        {
          if (strcmp (sender, request->sender) == 0)
            list = g_slist_prepend (list, g_object_ref (request));
        }
    }
  G_UNLOCK (requests);

  for (l = list; l; l = l->next)
    {
      XdpRequest *request = l->data;

      REQUEST_AUTOLOCK (request);

      if (request->exported)
        {
          if (request->impl_request)
            xdp_dbus_impl_request_call_close_sync (request->impl_request, NULL, NULL);

          xdp_request_unexport (request);
        }
    }

  g_slist_free_full (list, g_object_unref);
  g_task_return_boolean (task, TRUE);
}

void
close_requests_for_sender (const char *sender)
{
  GTask *task;

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_strdup (sender), g_free);
  g_task_run_in_thread (task, xdp_close_requests_in_thread_func);
  g_object_unref (task);
}

