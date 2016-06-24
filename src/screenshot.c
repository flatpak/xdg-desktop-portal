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
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

extern XdpDocuments *documents;
extern char *documents_mountpoint;

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

static char *
register_document (const char *uri,
                   const char *app_id,
                   GError **error)
{
  g_autofree char *doc_id = NULL;
  g_autofree char *path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *dirname = NULL;
  GUnixFDList *fd_list = NULL;
  int fd, fd_in;
  g_autoptr(GFile) file = NULL;
  gboolean ret = FALSE;
  const char *permissions[5];
  g_autofree char *fuse_path = NULL;
  int i;

  if (app_id == NULL || *app_id == 0)
    return g_strdup (uri);

  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  basename = g_path_get_basename (path);
  dirname = g_path_get_dirname (path);

  fd = open (path, O_PATH | O_CLOEXEC);

  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to open %s", uri);
      return NULL;
    }

  fd_list = g_unix_fd_list_new ();
  fd_in = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (fd_in == -1)
    return NULL;

  i = 0;
  permissions[i++] = "read";
  permissions[i++] = "grant";
  permissions[i++] = NULL;

  ret = xdp_documents_call_add_sync (documents,
                                     g_variant_new_handle (fd_in),
                                     TRUE,
                                     TRUE,
                                     fd_list,
                                     &doc_id,
                                     NULL,
                                     NULL,
                                     error);
  g_object_unref (fd_list);

  if (!ret)
    return NULL;

  if (!xdp_documents_call_grant_permissions_sync (documents,
                                                  doc_id,
                                                  app_id,
                                                  permissions,
                                                  NULL,
                                                  error))
    return NULL;

  return g_build_filename (documents_mountpoint, doc_id, basename, NULL);
}

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
  GVariantBuilder b;
  g_autofree char *ruri = NULL;
  g_autoptr(GError) error = NULL;

  if (request == NULL)
    return;

  REQUEST_AUTOLOCK (request);

  if (strcmp (arg_uri, "") != 0)
    {
      ruri = register_document (arg_uri, request->app_id, &error);
      if (ruri == NULL)
        {
          g_warning ("Failed to register %s: %s\n", arg_uri, error->message);
          g_clear_error (&error);
          ruri = g_strdup ("");
        }
    }
  else
    ruri = g_strdup ("");

  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&b, "u", arg_response);
  g_variant_builder_add (&b, "s", ruri);
  g_variant_builder_add (&b, "@a{sv}", arg_options);

  if (request->exported)
    {
      if (!g_dbus_connection_emit_signal (g_dbus_proxy_get_connection (G_DBUS_PROXY (object)),
                                          request->sender,
                                          request->id,
                                          "org.freedesktop.portal.ScreenshotRequest",
                                          "Response",
                                          g_variant_builder_end (&b),
                                          &error))
        {
          g_warning ("Error emitting signal: %s\n", error->message);
          g_clear_error (&error);
        }

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
