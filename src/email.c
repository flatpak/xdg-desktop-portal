/*
 * Copyright © 2017 Red Hat, Inc
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

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "email.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Email Email;
typedef struct _EmailClass EmailClass;

struct _Email
{
  XdpEmailSkeleton parent_instance;
};

struct _EmailClass
{
  XdpEmailSkeletonClass parent_class;
};

static XdpImplEmail *impl;
static Email *email;

GType email_get_type (void) G_GNUC_CONST;
static void email_iface_init (XdpEmailIface *iface);

G_DEFINE_TYPE_WITH_CODE (Email, email, XDP_TYPE_EMAIL_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_EMAIL, email_iface_init));

static void
send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
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
send_email_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_email_call_send_email_finish (XDP_IMPL_EMAIL (source),
                                              &response,
                                              &results,
                                              result,
                                              &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static XdpOptionKey send_email_options[] = {
  { "address", G_VARIANT_TYPE_STRING },
  { "subject", G_VARIANT_TYPE_STRING },
  { "body", G_VARIANT_TYPE_STRING },
  { "attachments", G_VARIANT_TYPE_STRING_ARRAY },
};

static gboolean
handle_send_email (XdpEmail *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

g_print ("handle send email\n");
  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
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

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &options,
                      send_email_options, G_N_ELEMENTS (send_email_options));

  xdp_impl_email_call_send_email (impl,
                                  request->id,
                                  app_id,
                                  arg_parent_window,
                                  g_variant_builder_end (&options),
                                  NULL,
                                  send_email_done,
                                  g_object_ref (request));

  xdp_email_complete_send_email (object, invocation, request->id);

  return TRUE;
}

static void
email_iface_init (XdpEmailIface *iface)
{
  iface->handle_send_email = handle_send_email;
}

static void
email_init (Email *email)
{
  xdp_email_set_version (XDP_EMAIL (email), 1);
}

static void
email_class_init (EmailClass *klass)
{
}

GDBusInterfaceSkeleton *
email_create (GDBusConnection *connection,
              const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_email_proxy_new_sync (connection,
                                        G_DBUS_PROXY_FLAGS_NONE,
                                        dbus_name,
                                        DESKTOP_PORTAL_OBJECT_PATH,
                                        NULL,
                                        &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create email proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  email = g_object_new (email_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (email);
}
