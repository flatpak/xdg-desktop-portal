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

G_LOCK_DEFINE (request_by_handle);
static GHashTable *request_by_handle;

static void
register_handle (const char *handle, Request *request)
{
  G_LOCK (request_by_handle);
  g_hash_table_insert (request_by_handle, g_strdup (handle), g_object_ref (request));
  G_UNLOCK (request_by_handle);
}

static void
unregister_handle (const char *handle)
{
  G_LOCK (request_by_handle);
  g_hash_table_remove (request_by_handle, handle);
  G_UNLOCK (request_by_handle);
}

static Request *
lookup_request_by_handle (const char *handle)
{
  Request *request;

  G_LOCK (request_by_handle);
  request = g_hash_table_lookup (request_by_handle, handle);
  if (request)
    g_object_ref (request);
  G_UNLOCK (request_by_handle);

  return request;
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              Request *request)
{
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (request->exported)
    {
      const char *handle = g_object_get_data (G_OBJECT (request), "impl-handle");

      if (!xdp_impl_print_call_close_sync (impl,
                                           request->sender, request->app_id, handle,
                                           NULL, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }

      unregister_handle (handle);
      request_unexport (request);
    }

  xdp_request_complete_close (XDP_REQUEST (request), invocation);

  return TRUE;
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
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  g_autofree char *impl_handle = NULL;

  if (!xdp_impl_print_call_print_file_sync (impl,
                                            sender, app_id,
                                            arg_parent_window,
                                            arg_title,
                                            arg_filename,
                                            arg_options,
                                            &impl_handle,
                                            NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_object_set_data_full (G_OBJECT (request), "impl-handle", g_strdup (impl_handle), g_free);
  register_handle (impl_handle, request);

  g_signal_connect (request, "handle-close", (GCallback)handle_close, request);

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_print_complete_print_file (object, invocation, request->id);
  return TRUE;
}

static void
handle_print_file_response (XdpImplPrint *object,
                            const gchar *arg_destination,
                            const gchar *arg_handle,
                            guint arg_response,
                            GVariant *arg_options)
{
  g_autoptr(Request) request = lookup_request_by_handle (arg_handle);

  if (request == NULL)
    return;

  REQUEST_AUTOLOCK (request);

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 arg_response,
                                 arg_options);
      unregister_handle (arg_handle);
      request_unexport (request);
    }
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

  request_by_handle = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, g_object_unref);

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

  set_proxy_use_threads (G_DBUS_PROXY (impl));

  print = g_object_new (print_get_type (), NULL);

  g_signal_connect (impl, "print-file-response", (GCallback)handle_print_file_response, NULL);

  return G_DBUS_INTERFACE_SKELETON (print);
}
