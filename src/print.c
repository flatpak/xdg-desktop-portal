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
#include "xdp-utils.h"

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
print_done (GObject *source,
            GAsyncResult *result,
            gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (!xdp_impl_print_call_print_finish (XDP_IMPL_PRINT (source),
                                         &response,
                                         &options,
                                         NULL,
                                         result,
                                         &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  if (request->exported)
    {
      GVariantBuilder opt_builder;

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }
}

static XdpOptionKey print_options[] = {
  { "token", G_VARIANT_TYPE_UINT32 },
  { "modal", G_VARIANT_TYPE_BOOLEAN }
};

static gboolean
handle_print (XdpPrint *object,
              GDBusMethodInvocation *invocation,
              GUnixFDList *fd_list,
              const gchar *arg_parent_window,
              const gchar *arg_title,
              GVariant *arg_fd,
              GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;

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

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &opt_builder,
                      print_options, G_N_ELEMENTS (print_options));
  xdp_impl_print_call_print(impl,
                            request->id,
                            app_id,
                            arg_parent_window,
                            arg_title,
                            arg_fd,
                            g_variant_builder_end (&opt_builder),
                            fd_list,
                            NULL,
                            print_done,
                            g_object_ref (request));

  xdp_print_complete_print (object, invocation, NULL, request->id);

  return TRUE;
}

XdpOptionKey response_options[] = {
  { "settings", G_VARIANT_TYPE_VARDICT },
  { "page-setup", G_VARIANT_TYPE_VARDICT },
  { "token", G_VARIANT_TYPE_UINT32 }
};

static void
prepare_print_done (GObject *source,
                    GAsyncResult *result,
                    gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) ret = NULL;

  REQUEST_AUTOLOCK (request);

  if (!xdp_impl_print_call_prepare_print_finish (XDP_IMPL_PRINT (source),
                                                 &response,
                                                 &options,
                                                 result,
                                                 &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  if (request->exported)
    {
      GVariantBuilder opt_builder;

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

      if (response == 0)
        xdp_filter_options (options, &opt_builder,
                            response_options, G_N_ELEMENTS (response_options));

      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&opt_builder));

      request_unexport (request);
    }
}

static XdpOptionKey prepare_print_options[] = {
  { "modal", G_VARIANT_TYPE_BOOLEAN }
};

static gboolean
handle_prepare_print (XdpPrint *object,
                      GDBusMethodInvocation *invocation,
                      const gchar *arg_parent_window,
                      const gchar *arg_title,
                      GVariant *arg_settings,
                      GVariant *arg_page_setup,
                      GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;

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

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &opt_builder,
                      prepare_print_options, G_N_ELEMENTS (prepare_print_options));
  xdp_impl_print_call_prepare_print (impl,
                                     request->id,
                                     app_id,
                                     arg_parent_window,
                                     arg_title,
                                     arg_settings,
                                     arg_page_setup,
                                     g_variant_builder_end (&opt_builder),
                                     NULL,
                                     prepare_print_done,
                                     g_object_ref (request));

  xdp_print_complete_prepare_print (object, invocation, request->id);

  return TRUE;
}

static void
print_iface_init (XdpPrintIface *iface)
{
  iface->handle_print = handle_print;
  iface->handle_prepare_print = handle_prepare_print;
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
              const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_print_proxy_new_sync (connection,
                                        G_DBUS_PROXY_FLAGS_NONE,
                                        dbus_name,
                                        DESKTOP_PORTAL_OBJECT_PATH,
                                        NULL,
                                        &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create print proxy: %s", error->message);
      return NULL;
    }

  print = g_object_new (print_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (print);
}
