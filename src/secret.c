/*
 * Copyright © 2019 Red Hat, Inc
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
 *       Daiki Ueno <dueno@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "secret.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Secret Secret;
typedef struct _SecretClass SecretClass;

struct _Secret
{
  XdpSecretSkeleton parent_instance;
};

struct _SecretClass
{
  XdpSecretSkeletonClass parent_class;
};

static XdpImplSecret *impl;
static Secret *secret;

GType secret_get_type (void) G_GNUC_CONST;
static void secret_iface_init (XdpSecretIface *iface);

G_DEFINE_TYPE_WITH_CODE (Secret, secret, XDP_TYPE_SECRET_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SECRET, secret_iface_init));

static XdpOptionKey retrieve_secret_options[] = {
  { "token", G_VARIANT_TYPE_STRING, NULL },
};

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  Request *request = task_data;
  guint response;
  GVariantBuilder new_results;

  g_variant_builder_init (&new_results, G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&new_results));
      request_unexport (request);
    }
}

static void
retrieve_secret_done (GObject *source,
		      GAsyncResult *result,
		      gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_secret_call_retrieve_secret_finish (XDP_IMPL_SECRET (source),
						    &response,
						    &results,
						    NULL,
						    result,
						    &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
handle_retrieve_secret (XdpSecret *object,
			GDBusMethodInvocation *invocation,
			GUnixFDList *fd_list,
			GVariant *arg_fd,
			GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

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

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);

  if (!xdp_filter_options (arg_options, &options,
                           retrieve_secret_options, G_N_ELEMENTS (retrieve_secret_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_secret_complete_retrieve_secret (object, invocation, NULL, request->id);

  xdp_impl_secret_call_retrieve_secret (impl,
					request->id,
					app_id,
					arg_fd,
					g_variant_builder_end (&options),
					fd_list,
					NULL,
					retrieve_secret_done,
					g_object_ref (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
secret_iface_init (XdpSecretIface *iface)
{
  iface->handle_retrieve_secret = handle_retrieve_secret;
}

static void
secret_init (Secret *secret)
{
  xdp_secret_set_version (XDP_SECRET (secret), 1);
}

static void
secret_class_init (SecretClass *klass)
{
}

GDBusInterfaceSkeleton *
secret_create (GDBusConnection *connection,
	       const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_secret_proxy_new_sync (connection,
					 G_DBUS_PROXY_FLAGS_NONE,
					 dbus_name,
					 DESKTOP_PORTAL_OBJECT_PATH,
					 NULL,
					 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create secret proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  secret = g_object_new (secret_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (secret);
}
