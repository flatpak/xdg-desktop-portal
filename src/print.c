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
#include <gio/gunixfdlist.h>

#include "print.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _Print Print;
typedef struct _PrintClass PrintClass;

struct _Print
{
  XdpPrintSkeleton parent_instance;
};

struct _PrintClass
{
  XdpPrintSkeletonClass parent_class;
};

static XdpImplPrint *impl;
static Print *print;

GType print_get_type (void) G_GNUC_CONST;
static void print_iface_init (XdpPrintIface *iface);

G_DEFINE_TYPE_WITH_CODE (Print, print, XDP_TYPE_PRINT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_PRINT, print_iface_init));

static void
print_file_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  g_autoptr(Request) request = data;
  guint response;
  GVariant *options;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (!xdp_impl_print_call_print_file_finish (XDP_IMPL_PRINT (source),
                                              &response, &options,
                                              result, &error))
    {
      response = 2;
      options = NULL;
    }

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request), response, options);
      request_unexport (request);
    }
}

static gboolean
handle_print_file (XdpPrint *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  const gchar *arg_filename,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;

  REQUEST_AUTOLOCK (request);

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

  xdp_impl_print_call_print_file (impl,
                                  request->id,
                                  app_id,
                                  arg_parent_window,
                                  arg_title,
                                  arg_filename,
                                  arg_options,
                                  NULL,
                                  print_file_done,
                                  g_object_ref (request));

  xdp_print_complete_print_file (object, invocation, request->id);

  return TRUE;
}

static void
print_iface_init (XdpPrintIface *iface)
{
  iface->handle_print_file = handle_print_file;
}

static void
print_init (Print *fc)
{
}

static void
print_class_init (PrintClass *klass)
{
}

GDBusInterfaceSkeleton *
print_create (GDBusConnection *connection,
              const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_print_proxy_new_sync (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         dbus_name,
                                         "/org/freedesktop/portal/desktop",
                                         NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create print proxy: %s\n", error->message);
      return NULL;
    }

  print = g_object_new (print_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (print);
}
