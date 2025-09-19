/*
 * Copyright © 2019 Red Hat, Inc
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

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-utils.h"

#include "secret.h"

typedef struct _Secret Secret;
typedef struct _SecretClass SecretClass;

struct _Secret
{
  XdpDbusSecretSkeleton parent_instance;

  XdpDbusImplSecret *impl;
};

struct _SecretClass
{
  XdpDbusSecretSkeletonClass parent_class;
};

GType secret_get_type (void) G_GNUC_CONST;
static void secret_iface_init (XdpDbusSecretIface *iface);

G_DEFINE_TYPE_WITH_CODE (Secret, secret, XDP_DBUS_TYPE_SECRET_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SECRET,
                                                secret_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Secret, g_object_unref)

static XdpOptionKey retrieve_secret_options[] = {
  { "token", G_VARIANT_TYPE_STRING, NULL },
};

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  XdpRequest *request = task_data;
  guint response;
  g_auto(GVariantBuilder) new_results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));

  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&new_results));
      xdp_request_unexport (request);
    }
}

static void
retrieve_secret_done (GObject *source,
		      GAsyncResult *result,
		      gpointer data)
{
  g_autoptr(XdpRequest) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_secret_call_retrieve_secret_finish (XDP_DBUS_IMPL_SECRET (source),
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
handle_retrieve_secret (XdpDbusSecret         *object,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList           *fd_list,
                        GVariant              *arg_fd,
                        GVariant              *arg_options)
{
  Secret *secret = (Secret *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (secret->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (secret->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options,
                           retrieve_secret_options, G_N_ELEMENTS (retrieve_secret_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_secret_complete_retrieve_secret (object, invocation, NULL, request->id);

  xdp_dbus_impl_secret_call_retrieve_secret (secret->impl,
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
secret_iface_init (XdpDbusSecretIface *iface)
{
  iface->handle_retrieve_secret = handle_retrieve_secret;
}

static void
secret_dispose (GObject *object)
{
  Secret *secret = (Secret *) object;

  g_clear_object (&secret->impl);

  G_OBJECT_CLASS (secret_parent_class)->dispose (object);
}

static void
secret_init (Secret *secret)
{
}

static void
secret_class_init (SecretClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = secret_dispose;
}

static Secret *
secret_new (XdpDbusImplSecret *impl)
{
  Secret *secret;

  secret = g_object_new (secret_get_type (), NULL);
  secret->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (secret->impl), G_MAXINT);

  xdp_dbus_secret_set_version (XDP_DBUS_SECRET (secret), 1);

  return secret;
}

static void
proxy_created_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  XdpContext *context;
  g_autoptr(XdpDbusImplSecret) impl = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(Secret) secret = NULL;

  impl = xdp_dbus_impl_secret_proxy_new_finish (result, &error);
  if (!impl)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to create secret proxy: %s", error->message);
      return;
    }

  context = XDP_CONTEXT (user_data);

  secret = secret_new (impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&secret)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}

void
init_secret (XdpContext   *context,
             GCancellable *cancellable)
{
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SECRET_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  xdp_dbus_impl_secret_proxy_new (connection,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  impl_config->dbus_name,
                                  DESKTOP_DBUS_PATH,
                                  cancellable,
                                  proxy_created_cb,
                                  context);
}
