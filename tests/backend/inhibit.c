#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "xdp-request.h"
#include "session.h"
#include "inhibit.h"

static GDBusInterfaceSkeleton *inhibit;

typedef struct {
  XdpDbusImplInhibit *impl;
  GDBusMethodInvocation *invocation;
  XdpRequest *request;
  GKeyFile *keyfile;
  char *app_id;
  guint flags;
  guint close_id;
  int timeout;
} InhibitHandle;

static void
inhibit_handle_free (InhibitHandle *handle)
{
  g_object_unref (handle->impl);
  if (handle->request)
    g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);

  if (handle->timeout)
    g_source_remove (handle->timeout);

  g_free (handle);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              gpointer data)
{
  InhibitHandle *handle = g_object_get_data (G_OBJECT (object), "handle");

  if (object->exported)
    xdp_request_unexport (object);

  xdp_dbus_impl_request_complete_close (XDP_DBUS_IMPL_REQUEST (object), invocation);

  g_debug ("Handling Close");

  if (handle)
    inhibit_handle_free (handle);
  else
    g_object_unref (object);

  return TRUE;
}

static gboolean
send_response (gpointer data)
{
  InhibitHandle *handle = data;
  int response;

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  if (response == 0)
    {
      xdp_dbus_impl_inhibit_complete_inhibit (handle->impl, handle->invocation);
      g_object_set_data (G_OBJECT (handle->request), "handle", NULL);
      handle->request = NULL;
    }
  else
    g_dbus_method_invocation_return_error (handle->invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "Canceled");

  handle->timeout = 0;

  inhibit_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_inhibit (XdpDbusImplInhibit *object,
                GDBusMethodInvocation *invocation,
                const char *arg_handle,
                const char *arg_app_id,
                const char *arg_parent_window,
                guint arg_flags,
                GVariant *arg_options)
{
  const char *sender;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  InhibitHandle *handle;
  g_autoptr(XdpRequest) request = NULL;
  int delay;

  g_debug ("Handling Inhibit");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "inhibit", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (arg_flags, ==, g_key_file_get_integer (keyfile, "inhibit", "flags", NULL));

  request = xdp_request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (InhibitHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->flags = arg_flags;

  g_object_set_data (G_OBJECT (request), "handle", handle);
  handle->close_id = g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), NULL);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));


  if (g_key_file_has_key (keyfile, "backend", "delay", NULL))
    delay = g_key_file_get_integer (keyfile, "backend", "delay", NULL);
  else
    delay = 200;

  g_debug ("delay %d", delay);

  if (delay == 0)
    send_response (handle);
  else
    handle->timeout = g_timeout_add (delay, send_response, handle);

  return TRUE;
}

typedef enum {
  UNKNOWN   = 0,
  RUNNING   = 1,
  QUERY_END = 2,
  ENDING    = 3
} SessionState;

static SessionState session_state = RUNNING;
static gboolean screensaver_active = FALSE;
static guint query_end_timeout;
static GList *active_sessions = NULL;

static void
emit_state_changed (XdpSession *session)
{
  GVariantBuilder state;

  g_debug ("Emitting StateChanged for session %s", session->id);

  g_variant_builder_init (&state, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&state, "{sv}", "screensaver-active", g_variant_new_boolean (screensaver_active));
  g_variant_builder_add (&state, "{sv}", "session-state", g_variant_new_uint32 (session_state));
  g_signal_emit_by_name (inhibit, "state-changed", session->id, g_variant_builder_end (&state));
}

typedef struct _InhibitSession
{
  XdpSession parent;
  gboolean pending_query_end_response;
} InhibitSession;

typedef struct _InhibitSessionClass
{
  XdpSessionClass parent_class;
} InhibitSessionClass;

GType inhibit_session_get_type (void);
G_DEFINE_TYPE (InhibitSession, inhibit_session, xdp_session_get_type ())

static void
global_set_pending_query_end_response (gboolean pending)
{
  GList *l;

  for (l = active_sessions; l; l = l->next)
    {
      InhibitSession *session = (InhibitSession *)l->data;
      session->pending_query_end_response = pending;
    }
}

static gboolean
global_get_pending_query_end_response (void)
{
  GList *l;

  for (l = active_sessions; l; l = l->next)
    {
      InhibitSession *session = (InhibitSession *)l->data;
      if (session->pending_query_end_response)
        return TRUE;
    }

  return FALSE;
}

static void
inhibit_session_close (XdpSession *session)
{
  InhibitSession *inhibit_session = (InhibitSession *)session;

  g_debug ("Closing inhibit session %s", ((XdpSession *)inhibit_session)->id);

  active_sessions = g_list_remove (active_sessions, session);
}

static void
inhibit_session_finalize (GObject *object)
{
  G_OBJECT_CLASS (inhibit_session_parent_class)->finalize (object);
}

static void
inhibit_session_init (InhibitSession *inhibit_session)
{
}

static void
inhibit_session_class_init (InhibitSessionClass *klass)
{
  GObjectClass *gobject_class;
  XdpSessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = inhibit_session_finalize;

  session_class = (XdpSessionClass *)klass;
  session_class->close = inhibit_session_close;
}

static InhibitSession *
inhibit_session_new (const char *app_id,
                     const char *session_handle)
{
  InhibitSession *inhibit_session;

  g_debug ("Creating inhibit session %s", session_handle);

  inhibit_session = g_object_new (inhibit_session_get_type (),
                                  "id", session_handle,
                                  NULL);

  active_sessions = g_list_prepend (active_sessions, inhibit_session);

  return inhibit_session;
}

static void
global_emit_state_changed (void)
{
  GList *l;

  for (l = active_sessions; l; l = l->next)
    emit_state_changed ((XdpSession *)l->data);
}

static void
set_session_state (SessionState state)
{
  const char *names[] = {
    "Unknown", "Running", "Query-end", "Ending"
  };

  g_debug ("Session state now: %s", names[state]);

  session_state = state;

  global_emit_state_changed ();
}

static void global_set_pending_query_end_response (gboolean pending);
static gboolean global_get_pending_query_end_response (void);

static void
stop_waiting_for_query_end_response (gboolean send_response)
{
  g_debug ("Stop waiting for QueryEndResponse calls");

  if (query_end_timeout != 0)
    {
      g_source_remove (query_end_timeout);
      query_end_timeout = 0;
    }

  global_set_pending_query_end_response (FALSE);
}

static gboolean
query_end_response (gpointer data)
{
  g_debug ("1 second wait is over");

  stop_waiting_for_query_end_response (TRUE);

  return G_SOURCE_REMOVE;
}

static void
wait_for_query_end_response (gpointer data)
{
  if (query_end_timeout != 0)
    return; /* we're already waiting */

  g_debug ("Waiting for up to 1 second for QueryEndResponse calls");

  query_end_timeout = g_timeout_add (1000, query_end_response, data);

  global_set_pending_query_end_response (TRUE);
}

static void
maybe_send_quit_response (void)
{
  if (query_end_timeout == 0)
    return;

  if (global_get_pending_query_end_response ())
    return;

  g_debug ("No more pending QueryEndResponse calls");

  stop_waiting_for_query_end_response (TRUE);
}

static gboolean
change_session_state (gpointer data)
{
  g_autoptr(GKeyFile) keyfile = data;
  g_autofree char *change = NULL;

  change = g_key_file_get_string (keyfile, "backend", "change", NULL);

  g_debug ("change session state: %s\n", change);

  if (change && g_str_has_prefix (change, "query-end"))
    {
      wait_for_query_end_response (NULL);
      set_session_state (QUERY_END);
      maybe_send_quit_response ();
    }

  return G_SOURCE_REMOVE;
}

static gboolean
handle_create_monitor (XdpDbusImplInhibit *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       const char *arg_window)
{
  g_autoptr(GError) error = NULL;
  int response;
  XdpSession *session;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  int delay;

  g_debug ("Handling CreateMonitor");

  session_state = RUNNING;
  screensaver_active = FALSE;

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "inhibit", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  session = (XdpSession *)inhibit_session_new (arg_app_id, arg_session_handle);

  if (!xdp_session_export (session, g_dbus_method_invocation_get_connection (invocation), &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create inhibit session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

  delay = g_key_file_get_integer (keyfile, "backend", "delay", NULL);

  g_debug ("delay %d", delay);

  if (delay != 0)
    g_timeout_add (delay, change_session_state, g_key_file_ref (keyfile));

out:
  xdp_dbus_impl_inhibit_complete_create_monitor (object, invocation, response);
  if (session)
    emit_state_changed (session);

  return TRUE;
}

static gboolean
handle_query_end_response (XdpDbusImplInhibit *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_session_handle)
{
  InhibitSession *session = (InhibitSession *)lookup_session (arg_session_handle);

  g_debug ("Handle QueryEndResponse for session %s", arg_session_handle);

  if (session)
    {
      session->pending_query_end_response = FALSE;
      maybe_send_quit_response ();
    }

  xdp_dbus_impl_inhibit_complete_query_end_response (object, invocation);

  return TRUE;
}

void
inhibit_init (GDBusConnection *connection,
              const char *object_path)
{
  g_autoptr(GError) error = NULL;

  inhibit = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_inhibit_skeleton_new ());

  g_signal_connect (inhibit, "handle-inhibit", G_CALLBACK (handle_inhibit), NULL);
  g_signal_connect (inhibit, "handle-create-monitor", G_CALLBACK (handle_create_monitor), NULL);
  g_signal_connect (inhibit, "handle-query-end-response", G_CALLBACK (handle_query_end_response), NULL);

  if (!g_dbus_interface_skeleton_export (inhibit, connection, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (inhibit)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (inhibit)->name, object_path);
}
