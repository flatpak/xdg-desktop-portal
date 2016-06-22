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
#include <gio/gdesktopappinfo.h>

#include "open-uri.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _OpenURI OpenURI;

typedef struct _OpenURIClass OpenURIClass;

struct _OpenURI
{
  XdpOpenURISkeleton parent_instance;
};

struct _OpenURIClass
{
  XdpOpenURISkeletonClass parent_class;
};

static XdpImplAppChooser *app_chooser_impl;
static OpenURI *open_uri;

GType open_uri_get_type (void) G_GNUC_CONST;
static void open_uri_iface_init (XdpOpenURIIface *iface);

G_DEFINE_TYPE_WITH_CODE (OpenURI, open_uri, XDP_TYPE_OPEN_URI_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_OPEN_URI, open_uri_iface_init));

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
      const char *handle = g_object_get_data (G_OBJECT (request), "app-chooser-impl-handle");

      if (!xdp_impl_app_chooser_call_close_sync (app_chooser_impl,
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
handle_open_uri (XdpOpenURI *object,
                 GDBusMethodInvocation *invocation,
                 const gchar *arg_parent_window,
                 const gchar *arg_uri,
                 GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  g_autofree char *app_chooser_impl_handle = NULL;
  g_auto(GStrv) choices = NULL;
  GList *infos, *l;
  g_autofree char *uri_scheme = NULL;
  g_autofree char *content_type = NULL;
  int i;

  uri_scheme = g_uri_parse_scheme (arg_uri);
  if (uri_scheme && uri_scheme[0] != '\0' && strcmp (uri_scheme, "file") != 0)
    {
      g_autofree char *scheme_down = g_ascii_strdown (uri_scheme, -1);
      content_type = g_strconcat ("x-scheme-handler/", scheme_down, NULL);
    }
  else
    {
      g_autoptr(GFile) file = g_file_new_for_uri (arg_uri);
      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                     0,
                                                     NULL,
                                                     NULL);
      content_type = g_strdup (g_file_info_get_content_type (info));
    }

  infos = g_app_info_get_recommended_for_type (content_type);
  choices = g_new (char *, g_list_length (infos) + 1);
  for (l = infos, i = 0; l; l = l->next)
    {
      GAppInfo *info = l->data;
      choices[i++] = g_strdup (g_app_info_get_id (info));
    }
  choices[i] = NULL;
  g_list_free_full (infos, g_object_unref);

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);

  if (!xdp_impl_app_chooser_call_choose_application_sync (app_chooser_impl,
                                                          sender, app_id,
                                                          arg_parent_window,
                                                          (const char * const *)choices,
                                                          arg_options,
                                                          &app_chooser_impl_handle,
                                                          NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_object_set_data_full (G_OBJECT (request), "app-chooser-impl-handle", g_strdup (app_chooser_impl_handle), g_free);
  register_handle (app_chooser_impl_handle, request);

  g_signal_connect (request, "handle-close", (GCallback)handle_close, request);

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_open_uri_complete_open_uri (object, invocation, request->id);
  return TRUE;
}

static void
handle_choose_application_response (XdpImplAppChooser *object,
                                    const gchar *arg_destination,
                                    const gchar *arg_handle,
                                    guint arg_response,
                                    const gchar *arg_choice,
                                    GVariant *arg_options)
{
  g_autoptr(Request) request = lookup_request_by_handle (arg_handle);
  GVariantBuilder b;
  g_autoptr(GError) error = NULL;

  if (request == NULL)
    return;

  REQUEST_AUTOLOCK (request);

  if (arg_response == 0)
    {
      g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (arg_choice));
      g_autoptr(GAppLaunchContext) context = g_app_launch_context_new ();
      const char *uri;
      const char *parent_window;
      GList uris;

      uri = g_object_get_data (G_OBJECT (request), "uri");
      parent_window = g_object_get_data (G_OBJECT (request), "parent-window");

      g_app_launch_context_setenv (context, "PARENT_WINDOW_ID", parent_window);

      uris.data = (gpointer)uri;
      uris.next = NULL;

      g_debug ("launching %s\n", arg_choice);
      g_app_info_launch_uris (info, &uris, context, NULL);
    }

  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&b, "u", arg_response);
  g_variant_builder_add (&b, "@a{sv}", arg_options);

  if (request->exported)
    {
      if (!g_dbus_connection_emit_signal (g_dbus_proxy_get_connection (G_DBUS_PROXY (object)),
                                          request->sender,
                                          request->id,
                                          "org.freedesktop.portal.OpenURIRequest",
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
open_uri_iface_init (XdpOpenURIIface *iface)
{
  iface->handle_open_uri = handle_open_uri;
}

static void
open_uri_init (OpenURI *fc)
{
}

static void
open_uri_class_init (OpenURIClass *klass)
{
}

GDBusInterfaceSkeleton *
open_uri_create (GDBusConnection *connection,
                 const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  request_by_handle = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, g_object_unref);

  app_chooser_impl = xdp_impl_app_chooser_proxy_new_sync (connection,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          dbus_name,
                                                          "/org/freedesktop/portal/desktop",
                                                          NULL, &error);
  if (app_chooser_impl == NULL)
    {
      g_warning ("Failed to create app chooser proxy: %s\n", error->message);
      return NULL;
    }

  set_proxy_use_threads (G_DBUS_PROXY (app_chooser_impl));

  open_uri = g_object_new (open_uri_get_type (), NULL);

  g_signal_connect (app_chooser_impl, "choose-application-response", (GCallback)handle_choose_application_response, NULL);

  return G_DBUS_INTERFACE_SKELETON (open_uri);
}

