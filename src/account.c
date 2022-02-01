/*
 * Copyright © 2016 Red Hat, Inc
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

#include "account.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Account Account;
typedef struct _AccountClass AccountClass;

struct _Account
{
  XdpAccountSkeleton parent_instance;
};

struct _AccountClass
{
  XdpAccountSkeletonClass parent_class;
};

static XdpImplAccount *impl;
static Account *account;

GType account_get_type (void) G_GNUC_CONST;
static void account_iface_init (XdpAccountIface *iface);

G_DEFINE_TYPE_WITH_CODE (Account, account, XDP_TYPE_ACCOUNT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_ACCOUNT, account_iface_init));

static void
send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  Request *request = task_data;
  guint response;
  GVariant *results;
  GVariantBuilder new_results;
  g_autoptr(GVariant) idv = NULL;
  g_autoptr(GVariant) namev = NULL;
  const char *image;

  g_variant_builder_init (&new_results, G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  results = (GVariant *)g_object_get_data (G_OBJECT (request), "results");

  if (response != 0)
    goto out;

  idv = g_variant_lookup_value (results, "id", G_VARIANT_TYPE_STRING);
  namev = g_variant_lookup_value (results, "name", G_VARIANT_TYPE_STRING);

  g_variant_builder_add (&new_results, "{sv}", "id", idv);
  g_variant_builder_add (&new_results, "{sv}", "name", namev);

  if (g_variant_lookup (results, "image", "&s", &image))
    {
      g_autofree char *ruri = NULL;
      g_autoptr(GError) error = NULL;

      ruri = register_document (image, xdp_app_info_get_id (request->app_info), FALSE, FALSE, FALSE, &error);
      if (ruri == NULL)
        g_warning ("Failed to register %s: %s", image, error->message);
      else
        {
          g_debug ("convert uri '%s' -> '%s'\n", image, ruri);
          g_variant_builder_add (&new_results, "{sv}", "image", g_variant_new_string (ruri));
        }
    }

out:
  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&new_results));
      request_unexport (request);
    }
}

static void
get_user_information_done (GObject *source,
                           GAsyncResult *result,
                           gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_account_call_get_user_information_finish (XDP_IMPL_ACCOUNT (source),
                                                          &response,
                                                          &results,
                                                          result,
                                                          &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (results)
    g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
validate_reason (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (g_utf8_strlen (string, -1) > 256)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting overly long reasons");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey user_information_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason },
};

static gboolean
handle_get_user_information (XdpAccount *object,
                             GDBusMethodInvocation *invocation,
                             const gchar *arg_parent_window,
                             GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  g_debug ("Handling GetUserInformation");

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

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &options,
                      user_information_options, G_N_ELEMENTS (user_information_options),
                      NULL);

  g_debug ("options filtered");

  xdp_impl_account_call_get_user_information (impl,
                                              request->id,
                                              app_id,
                                              arg_parent_window,
                                              g_variant_builder_end (&options),
                                              NULL,
                                              get_user_information_done,
                                              g_object_ref (request));

  xdp_account_complete_get_user_information (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
account_iface_init (XdpAccountIface *iface)
{
  iface->handle_get_user_information = handle_get_user_information;
}

static void
account_init (Account *account)
{
  xdp_account_set_version (XDP_ACCOUNT (account), 1);
}

static void
account_class_init (AccountClass *klass)
{
}

GDBusInterfaceSkeleton *
account_create (GDBusConnection *connection,
                const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_account_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          dbus_name,
                                          DESKTOP_PORTAL_OBJECT_PATH,
                                          NULL,
                                          &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create account proxy: %s", error->message);
      return NULL;
    }

  g_debug ("using %s at %s\n", "org.freedesktop.impl.portal.Account", dbus_name);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  account = g_object_new (account_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (account);
}
