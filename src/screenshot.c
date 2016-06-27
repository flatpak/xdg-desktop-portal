#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "screenshot.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _Screenshot Screenshot;
typedef struct _ScreenshotClass ScreenshotClass;

struct _Screenshot
{
  XdpScreenshotSkeleton parent_instance;
};

struct _ScreenshotClass
{
  XdpScreenshotSkeletonClass parent_class;
};

static XdpImplScreenshot *impl;
static Screenshot *screenshot;

GType screenshot_get_type (void) G_GNUC_CONST;
static void screenshot_iface_init (XdpScreenshotIface *iface);

G_DEFINE_TYPE_WITH_CODE (Screenshot, screenshot, XDP_TYPE_SCREENSHOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SCREENSHOT, screenshot_iface_init));

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

      if (!xdp_impl_screenshot_call_close_sync (impl,
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
handle_screenshot (XdpScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  g_autofree char *impl_handle = NULL;

  if (!xdp_impl_screenshot_call_screenshot_sync (impl,
                                                 sender, app_id,
                                                 arg_parent_window,
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

  xdp_screenshot_complete_screenshot (object, invocation, request->id);

  return TRUE;
}

static void
handle_screenshot_response (XdpImplScreenshot *object,
                            const gchar *arg_destination,
                            const gchar *arg_handle,
                             guint arg_response,
                             const gchar *arg_uri,
                             GVariant *arg_options)
{
  g_autoptr(Request) request = lookup_request_by_handle (arg_handle);
  GVariantBuilder results;
  g_autofree char *ruri = NULL;
  g_autoptr(GError) error = NULL;
  int n_children;
  int i;

  if (request == NULL)
    return;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  n_children = g_variant_n_children (arg_options);
  for (i = 0; i < n_children; i++)
    g_variant_builder_add_value (&results, g_variant_get_child_value (arg_options, i));

  if (strcmp (arg_uri, "") != 0)
    {
      ruri = register_document (arg_uri, request->app_id, FALSE, FALSE, &error);
      if (ruri == NULL)
        {
          g_warning ("Failed to register %s: %s\n", arg_uri, error->message);
          g_clear_error (&error);
        }
      else
        g_variant_builder_add (&results, "{&sv}", "uri", g_variant_new_string (ruri));
    }

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 arg_response,
                                 g_variant_builder_end (&results));

      unregister_handle (arg_handle);
      request_unexport (request);
    }
}

static void
screenshot_iface_init (XdpScreenshotIface *iface)
{
  iface->handle_screenshot = handle_screenshot;
}

static void
screenshot_init (Screenshot *fc)
{
}

static void
screenshot_class_init (ScreenshotClass *klass)
{
}

GDBusInterfaceSkeleton *
screenshot_create (GDBusConnection *connection,
                   const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  request_by_handle = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, g_object_unref);

  impl = xdp_impl_screenshot_proxy_new_sync (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             dbus_name,
                                             "/org/freedesktop/portal/desktop",
                                             NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create screenshot proxy: %s\n", error->message);
      return NULL;
    }

  set_proxy_use_threads (G_DBUS_PROXY (impl));

  screenshot = g_object_new (screenshot_get_type (), NULL);

  g_signal_connect (impl, "screenshot-response", (GCallback)handle_screenshot_response, NULL);

  return G_DBUS_INTERFACE_SKELETON (screenshot);
}
