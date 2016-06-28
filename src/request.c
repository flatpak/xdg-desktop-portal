#include "request.h"

#include <string.h>

static void request_skeleton_iface_init (XdpRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (Request, request, XDP_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_REQUEST, request_skeleton_iface_init))

static void
request_on_signal_response (XdpRequest *object,
                            guint arg_response,
                            GVariant *arg_results)
{
  Request *request = (Request *)object;
  XdpRequestSkeleton *skeleton = XDP_REQUEST_SKELETON (object);
  GList      *connections, *l;
  GVariant   *signal_variant;

  connections = g_dbus_interface_skeleton_get_connections (G_DBUS_INTERFACE_SKELETON (skeleton));

  signal_variant = g_variant_ref_sink (g_variant_new ("(u@a{sv})",
                                                      arg_response,
                                                      arg_results));
  for (l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     request->sender,
                                     g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)),
                                     "org.freedesktop.portal.Request",
                                     "Response",
                                     signal_variant,
                                     NULL);
    }
  g_variant_unref (signal_variant);
  g_list_free_full (connections, g_object_unref);
}

static void
request_skeleton_iface_init (XdpRequestIface *iface)
{
  iface->response = request_on_signal_response;
}

G_LOCK_DEFINE (requests);
static GHashTable *requests;

static void
request_init (Request *request)
{
  g_mutex_init (&request->mutex);
}

static void
request_finalize (GObject *object)
{
  Request *request = (Request *)object;

  xdp_unregister_request (request);

  G_LOCK (requests);
  g_hash_table_remove (requests, request->id);
  G_UNLOCK (requests);

  g_free (request->app_id);
  g_free (request->sender);
  g_free (request->id);
  g_mutex_clear (&request->mutex);
  G_OBJECT_CLASS (request_parent_class)->finalize (object);
}

static void
request_class_init (RequestClass *klass)
{
  GObjectClass *gobject_class;

  requests = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize  = request_finalize;
}

static gboolean
request_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  const gchar *request_sender = user_data;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (strcmp (sender, request_sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

void
request_init_invocation (GDBusMethodInvocation  *invocation, const char *app_id)
{
  Request *request;
  guint32 r;
  char *id = NULL;

  request = g_object_new (request_get_type (), NULL);
  request->app_id = g_strdup (app_id);
  request->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  G_LOCK (requests);

  r = g_random_int ();
  do
    {
      g_free (id);
      id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%u", r);
    }
  while (g_hash_table_lookup (requests, id) != NULL);

  request->id = id;
  g_hash_table_insert (requests, id, request);

  G_UNLOCK (requests);

  xdp_register_request (request);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (request),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request->sender);


  g_object_set_data_full (G_OBJECT (invocation), "request", request, g_object_unref);
}

Request *
request_from_invocation (GDBusMethodInvocation *invocation)
{
  return g_object_get_data (G_OBJECT (invocation), "request");
}

void
request_export (Request *request,
                GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         connection,
                                         request->id,
                                         &error))
    {
      g_warning ("error exporting request: %s\n", error->message);
      g_clear_error (&error);
    }

  g_object_ref (request);
  request->exported = TRUE;
}

void
request_unexport (Request *request)
{
  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}

typedef struct {
  gchar *sender_name;
  gchar *signal_name;
  GVariant *parameters;
} SignalData;

static void
signal_data_free (SignalData *data)
{
  g_free (data->sender_name);
  g_free (data->signal_name);
  g_variant_unref (data->parameters);
  g_free (data);
}

static void
dispatch_in_thread_func (GTask        *task,
                         gpointer      source_object,
                         gpointer      task_data,
                         GCancellable *cancellable)
{
  SignalData *data = task_data;
  GDBusProxy *proxy = source_object;

  (G_DBUS_PROXY_GET_CLASS(proxy)->g_signal) (proxy,
                                             data->sender_name,
                                             data->signal_name,
                                             data->parameters);
}

static void
proxy_g_signal_cb (GDBusProxy *proxy,
                   const gchar *sender_name,
                   const gchar *signal_name,
                   GVariant *parameters)
{
  GTask *task;
  SignalData *data;

  data = g_new0 (SignalData, 1);
  data->sender_name = g_strdup (sender_name);
  data->signal_name = g_strdup (signal_name);
  data->parameters = g_variant_ref (parameters);

  task = g_task_new (proxy, NULL, NULL, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify) signal_data_free);
  g_task_run_in_thread (task, dispatch_in_thread_func);
  g_object_unref (task);

  g_signal_stop_emission_by_name (proxy, "g-signal");
}

void
set_proxy_use_threads (GDBusProxy *proxy)
{
  g_signal_connect (proxy, "g-signal", (GCallback)proxy_g_signal_cb, NULL);
}
