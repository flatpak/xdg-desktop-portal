/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#include "xdp-wp-app-audio-capture.h"

#include <libdex.h>
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/string.h>

#include "xdp-utils.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-app-audio-capture");

#define LINK_GROUP_PREFIX "xdp-app-audio-"
#define OUTPUT_MEDIA_CLASS "Stream/Output/Audio"
#define INPUT_MEDIA_CLASS "Stream/Input/Audio"
#define START_TIMEOUT_MS 1000
#define POSITIONS_7_1 "[ FL, FR, FC, LFE, RL, RR, SL, SR ]"

#define XDP_WP_TYPE_APP_AUDIO_LOOPBACK (xdp_wp_app_audio_loopback_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioLoopback, xdp_wp_app_audio_loopback, XDP_WP, APP_AUDIO_LOOPBACK,
                      GObject)

#define XDP_WP_TYPE_APP_AUDIO_STREAM (xdp_wp_app_audio_stream_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioStream, xdp_wp_app_audio_stream, XDP_WP, APP_AUDIO_STREAM,
                      GObject)

#define XDP_WP_TYPE_APP_AUDIO_SESSION (xdp_wp_app_audio_session_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioSession, xdp_wp_app_audio_session, XDP_WP, APP_AUDIO_SESSION,
                      XdpDbusImplSessionSkeleton)


struct _XdpWpAppAudioLoopback
{
  GObject parent_instance;

  WpCore *core;
  WpImplModule *module;
  WpObjectManager *nodes;
  WpNode *capture;
  WpNode *playback;
  GSource *closed_source;
};

G_DEFINE_FINAL_TYPE (XdpWpAppAudioLoopback, xdp_wp_app_audio_loopback, G_TYPE_OBJECT)

enum
{
  LOOPBACK_CLOSED,
  N_LOOPBACK_SIGNALS,
};

static guint loopback_signals[N_LOOPBACK_SIGNALS];

static char *
loopback_args (const char *link_group,
               guint32     id)
{
  g_autofree char *playback_name = g_strdup_printf ("xdp-app-audio-playback-%08x", id);
  g_autofree char *capture_name = g_strdup_printf ("xdp-app-audio-capture-%08x", id);
  g_autoptr (WpSpaJson) positions = wp_spa_json_new_from_string (POSITIONS_7_1);
  g_autoptr (WpSpaJson) capture = NULL;
  g_autoptr (WpSpaJson) playback = NULL;
  g_autoptr (WpSpaJson) args = NULL;

  /* Both nodes are not autoconnected, so WirePlumber doesn't link the microphone into the
   * capture node and the playback node to the speakers.
   * The nodes have a 7.1 channel layout and with stream.dont-remix=true, so WirePlumber
   * doesn't reconfigure the capture node's ports to the layout of the first stream linked into it,
   * which would, for example, drop channels from surround streams.
   * Readers ask for the channel layout they want when linking to the playback node, and
   * PipeWire downmixes it as needed. */
  capture = wp_spa_json_new_object (PW_KEY_NODE_NAME, "s", capture_name,
                                    PW_KEY_NODE_DESCRIPTION, "s", "App audio capture",
                                    SPA_KEY_AUDIO_POSITION, "J", positions,
                                    PW_KEY_STREAM_DONT_REMIX, "b", TRUE,
                                    PW_KEY_NODE_AUTOCONNECT, "b", FALSE,
                                    NULL);
  playback = wp_spa_json_new_object (PW_KEY_NODE_NAME, "s", playback_name,
                                     PW_KEY_NODE_DESCRIPTION, "s", "App audio",
                                     PW_KEY_NODE_AUTOCONNECT, "b", FALSE,
                                     NULL);
  args = wp_spa_json_new_object (SPA_KEY_AUDIO_POSITION, "J", positions,
                                 PW_KEY_NODE_LINK_GROUP, "s", link_group,
                                 "capture.props", "J", capture,
                                 "playback.props", "J", playback,
                                 NULL);
  return wp_spa_json_to_string (args);
}

static WpNode *
lookup_node (WpObjectManager *nodes,
             const char      *media_class)
{
  return wp_object_manager_lookup (nodes, WP_TYPE_NODE,
                                   WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                   PW_KEY_MEDIA_CLASS, "=s", media_class,
                                   NULL);
}

static void
on_nodes_changed (WpObjectManager *nodes,
                  DexPromise      *both_there)
{
  g_autoptr (WpNode) capture = lookup_node (nodes, INPUT_MEDIA_CLASS);
  g_autoptr (WpNode) playback = lookup_node (nodes, OUTPUT_MEDIA_CLASS);

  if (capture && playback && dex_future_is_pending (DEX_FUTURE (both_there)))
    dex_promise_resolve_boolean (both_there, TRUE);
}

static void
unref_promise (gpointer  data,
               GClosure *closure)
{
  dex_unref (data);
}

static gboolean
emit_closed_idle (gpointer user_data)
{
  XdpWpAppAudioLoopback *self = user_data;

  g_clear_pointer (&self->closed_source, g_source_unref);
  g_signal_emit (self, loopback_signals[LOOPBACK_CLOSED], 0);
  return G_SOURCE_REMOVE;
}

static void
on_node_removed (XdpWpAppAudioLoopback *self,
                 WpNode                *node)
{
  if (node != self->capture && node != self->playback)
    return;

  g_clear_object (&self->capture);
  g_clear_object (&self->playback);
  wp_core_idle_add (self->core, &self->closed_source, emit_closed_idle, self, NULL);
}

static DexFuture *
on_nodes_there (DexFuture *future,
                gpointer   user_data)
{
  XdpWpAppAudioLoopback *self = user_data;

  g_signal_handlers_disconnect_matched (self->nodes, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                        on_nodes_changed, NULL);
  self->capture = lookup_node (self->nodes, INPUT_MEDIA_CLASS);
  self->playback = lookup_node (self->nodes, OUTPUT_MEDIA_CLASS);
  if (!self->capture || !self->playback)
    return dex_future_new_reject (XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                  "The loopback's nodes went away");

  g_signal_connect_object (self->nodes, "object-removed",
                           G_CALLBACK (on_node_removed), self, G_CONNECT_SWAPPED);
  return dex_future_new_for_object (self);
}

static DexFuture *
xdp_wp_app_audio_loopback_new_future (WpCore *core)
{
  g_autoptr (XdpWpAppAudioLoopback) self = g_object_new (XDP_WP_TYPE_APP_AUDIO_LOOPBACK, NULL);
  g_autoptr (DexPromise) both_there = dex_promise_new ();
  g_autofree char *args = NULL;
  static guint32 next_id;
  guint32 id = next_id++;
  g_autofree char *link_group = NULL;

  link_group = g_strdup_printf (LINK_GROUP_PREFIX "%08x", id);

  self->core = g_object_ref (core);
  self->nodes = wp_object_manager_new ();
  wp_object_manager_add_interest (self->nodes, WP_TYPE_NODE,
                                  WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                  PW_KEY_NODE_LINK_GROUP, "=s", link_group,
                                  NULL);
  wp_object_manager_request_object_features (self->nodes, WP_TYPE_NODE,
                                             WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  g_signal_connect_data (self->nodes, "objects-changed",
                         G_CALLBACK (on_nodes_changed), dex_ref (both_there),
                         unref_promise, G_CONNECT_DEFAULT);
  wp_core_install_object_manager (core, self->nodes);

  args = loopback_args (link_group, id);
  self->module = wp_impl_module_load (core, "libpipewire-module-loopback", args, NULL);
  if (!self->module)
    return dex_future_new_reject (XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                  "Can't load libpipewire-module-loopback");

  return dex_future_then (dex_future_first (dex_ref (both_there),
                                            dex_timeout_new_msec (START_TIMEOUT_MS),
                                            NULL),
                          on_nodes_there, g_steal_pointer (&self), g_object_unref);
}

static WpNode *
xdp_wp_app_audio_loopback_get_playback_node (XdpWpAppAudioLoopback *self)
{
  return self->playback;
}

static gboolean
xdp_wp_app_audio_loopback_is_closed (XdpWpAppAudioLoopback *self)
{
  return !self->playback;
}

static void
xdp_wp_app_audio_loopback_dispose (GObject *object)
{
  XdpWpAppAudioLoopback *self = XDP_WP_APP_AUDIO_LOOPBACK (object);

  if (self->closed_source)
    g_source_destroy (self->closed_source);
  g_clear_pointer (&self->closed_source, g_source_unref);
  g_clear_object (&self->nodes);
  g_clear_object (&self->capture);
  g_clear_object (&self->playback);
  g_clear_object (&self->module);
  g_clear_object (&self->core);

  G_OBJECT_CLASS (xdp_wp_app_audio_loopback_parent_class)->dispose (object);
}

static void
xdp_wp_app_audio_loopback_class_init (XdpWpAppAudioLoopbackClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_wp_app_audio_loopback_dispose;

  loopback_signals[LOOPBACK_CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
xdp_wp_app_audio_loopback_init (XdpWpAppAudioLoopback *self)
{
}

struct _XdpWpAppAudioStream
{
  GObject parent_instance;

  XdpWpAppAudioLoopback *loopback;
};

G_DEFINE_FINAL_TYPE (XdpWpAppAudioStream, xdp_wp_app_audio_stream, G_TYPE_OBJECT)

enum
{
  STREAM_CLOSED,
  N_STREAM_SIGNALS,
};

static guint stream_signals[N_STREAM_SIGNALS];

static void
emit_closed (XdpWpAppAudioStream *self)
{
  g_signal_emit (self, stream_signals[STREAM_CLOSED], 0);
}

static DexFuture *
on_loopback (DexFuture *future,
             gpointer   user_data)
{
  XdpWpAppAudioStream *self = g_object_new (XDP_WP_TYPE_APP_AUDIO_STREAM, NULL);

  self->loopback = g_value_dup_object (dex_future_get_value (future, NULL));
  g_signal_connect_object (self->loopback, "closed",
                           G_CALLBACK (emit_closed), self, G_CONNECT_SWAPPED);
  return dex_future_new_take_object (self);
}

static DexFuture *
xdp_wp_app_audio_stream_new_future (WpCore *core)
{
  return dex_future_then (xdp_wp_app_audio_loopback_new_future (core),
                          on_loopback, NULL, NULL);
}

static guint32
xdp_wp_app_audio_stream_get_node_id (XdpWpAppAudioStream *self)
{
  WpNode *playback = xdp_wp_app_audio_loopback_get_playback_node (self->loopback);

  return wp_proxy_get_bound_id (WP_PROXY (playback));
}

static guint64
xdp_wp_app_audio_stream_get_serial (XdpWpAppAudioStream *self)
{
  WpNode *playback = xdp_wp_app_audio_loopback_get_playback_node (self->loopback);
  const char *str = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (playback),
                                                     PW_KEY_OBJECT_SERIAL);
  guint64 serial;

  return str && spa_atou64 (str, &serial, 10) ? serial : 0;
}

static gboolean
xdp_wp_app_audio_stream_is_closed (XdpWpAppAudioStream *self)
{
  return xdp_wp_app_audio_loopback_is_closed (self->loopback);
}

static void
xdp_wp_app_audio_stream_dispose (GObject *object)
{
  XdpWpAppAudioStream *self = XDP_WP_APP_AUDIO_STREAM (object);

  g_clear_object (&self->loopback);

  G_OBJECT_CLASS (xdp_wp_app_audio_stream_parent_class)->dispose (object);
}

static void
xdp_wp_app_audio_stream_class_init (XdpWpAppAudioStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_wp_app_audio_stream_dispose;

  stream_signals[STREAM_CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
xdp_wp_app_audio_stream_init (XdpWpAppAudioStream *self)
{
}

struct _XdpWpAppAudioSession
{
  XdpDbusImplSessionSkeleton parent_instance;

  char *handle;
  char *owner;
  GPtrArray *streams; /* XdpWpAppAudioStream */
  gboolean closed;
};

static void xdp_wp_app_audio_session_iface_init (XdpDbusImplSessionIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpWpAppAudioSession,
                               xdp_wp_app_audio_session,
                               XDP_DBUS_IMPL_TYPE_SESSION_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_IMPL_TYPE_SESSION,
                                                      xdp_wp_app_audio_session_iface_init))

enum
{
  SESSION_ENDED,
  N_SESSION_SIGNALS,
};

static guint session_signals[N_SESSION_SIGNALS];

static void
xdp_wp_app_audio_session_close (XdpWpAppAudioSession *self,
                                gboolean              notify)
{
  if (self->closed)
    return;

  self->closed = TRUE;
  if (notify)
    xdp_dbus_impl_session_emit_closed (XDP_DBUS_IMPL_SESSION (self));
  if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self)))
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self));
  g_ptr_array_set_size (self->streams, 0);
  g_signal_emit (self, session_signals[SESSION_ENDED], 0);
}

static void
on_stream_closed (XdpWpAppAudioSession *self)
{
  xdp_wp_app_audio_session_close (self, TRUE);
}

static void
xdp_wp_app_audio_session_add_stream (XdpWpAppAudioSession *self,
                                     XdpWpAppAudioStream  *stream)
{
  g_ptr_array_add (self->streams, g_object_ref (stream));
  g_signal_connect_object (stream, "closed",
                           G_CALLBACK (on_stream_closed), self, G_CONNECT_SWAPPED);
}

static gboolean
handle_close (XdpDbusImplSession    *object,
              GDBusMethodInvocation *invocation)
{
  XdpWpAppAudioSession *self = XDP_WP_APP_AUDIO_SESSION (object);

  if (g_strcmp0 (g_dbus_method_invocation_get_sender (invocation), self->owner) != 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                                     "Not the session's creator");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_session_complete_close (object, invocation);
  xdp_wp_app_audio_session_close (self, FALSE);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_wp_app_audio_session_iface_init (XdpDbusImplSessionIface *iface)
{
  iface->handle_close = handle_close;
}

static void
xdp_wp_app_audio_session_finalize (GObject *object)
{
  XdpWpAppAudioSession *self = XDP_WP_APP_AUDIO_SESSION (object);

  g_clear_pointer (&self->streams, g_ptr_array_unref);
  g_clear_pointer (&self->owner, g_free);
  g_clear_pointer (&self->handle, g_free);

  G_OBJECT_CLASS (xdp_wp_app_audio_session_parent_class)->finalize (object);
}

static void
xdp_wp_app_audio_session_class_init (XdpWpAppAudioSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_wp_app_audio_session_finalize;

  /* Not "closed", the D-Bus Closed signal of the skeleton */
  session_signals[SESSION_ENDED] =
    g_signal_new ("ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
xdp_wp_app_audio_session_init (XdpWpAppAudioSession *self)
{
  self->streams = g_ptr_array_new_with_free_func (g_object_unref);
}

static XdpWpAppAudioSession *
xdp_wp_app_audio_session_new (const char *handle,
                              const char *owner)
{
  XdpWpAppAudioSession *self = g_object_new (XDP_WP_TYPE_APP_AUDIO_SESSION, NULL);

  self->handle = g_strdup (handle);
  self->owner = g_strdup (owner);
  xdp_dbus_impl_session_set_version (XDP_DBUS_IMPL_SESSION (self), 1);
  return self;
}

struct _XdpWpAppAudioCapture
{
  XdpDbusImplAppAudioCaptureSkeleton parent_instance;

  WpCore *core;
  GDBusConnection *connection;
  guint name_id;
  guint disconnect_id;
  GHashTable *sessions; /* char *handle -> XdpWpAppAudioSession *session */
};

static void xdp_wp_app_audio_capture_iface_init (XdpDbusImplAppAudioCaptureIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpWpAppAudioCapture,
                               xdp_wp_app_audio_capture,
                               XDP_DBUS_IMPL_TYPE_APP_AUDIO_CAPTURE_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_IMPL_TYPE_APP_AUDIO_CAPTURE,
                                                      xdp_wp_app_audio_capture_iface_init))

typedef enum
{
  PROP_CORE = 1,
  PROP_CONNECTION,
} XdpWpAppAudioCaptureProps;

static GParamSpec *props[PROP_CONNECTION + 1] = { NULL, };

static void
on_session_closed (XdpWpAppAudioCapture *self,
                   XdpWpAppAudioSession *session)
{
  g_hash_table_remove (self->sessions, session->handle);
}

static void
close_sessions (XdpWpAppAudioCapture *self,
                const char           *owner,
                gboolean              notify)
{
  g_autoptr (GPtrArray) sessions = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  XdpWpAppAudioSession *session;

  g_hash_table_iter_init (&iter, self->sessions);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &session))
    if (!owner || g_strcmp0 (session->owner, owner) == 0)
      g_ptr_array_add (sessions, g_object_ref (session));

  for (guint i = 0; i < sessions->len; i++)
    xdp_wp_app_audio_session_close (g_ptr_array_index (sessions, i), notify);
}

static void
on_peer_disconnect (const char *name,
                    gpointer    user_data)
{
  close_sessions (user_data, name, FALSE);
}

static XdpWpAppAudioSession *
lookup_caller_session (XdpWpAppAudioCapture  *self,
                       GDBusMethodInvocation *invocation,
                       const char            *handle)
{
  XdpWpAppAudioSession *session = g_hash_table_lookup (self->sessions, handle);

  if (!session || g_strcmp0 (session->owner, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "No session of the caller at %s", handle);
      return NULL;
    }
  return session;
}

static gboolean
handle_create_session (XdpDbusImplAppAudioCapture *object,
                       GDBusMethodInvocation      *invocation,
                       const char                 *arg_handle,
                       const char                 *arg_app_id,
                       GVariant                   *arg_options)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);
  g_autoptr (XdpWpAppAudioSession) session = NULL;
  g_autoptr (GError) error = NULL;

  if (g_hash_table_contains (self->sessions, arg_handle))
    {
      g_dbus_method_invocation_return_error (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_EXISTS,
                                             "A session exists at %s", arg_handle);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = xdp_wp_app_audio_session_new (arg_handle,
                                          g_dbus_method_invocation_get_sender (invocation));
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session), self->connection,
                                         arg_handle, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_signal_connect_object (session, "ended",
                           G_CALLBACK (on_session_closed), self, G_CONNECT_SWAPPED);
  g_hash_table_insert (self->sessions, g_strdup (arg_handle), g_steal_pointer (&session));
  xdp_dbus_impl_app_audio_capture_complete_create_session (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
create_stream (XdpWpAppAudioCapture  *self,
               GDBusMethodInvocation *invocation,
               const char            *session_handle)
{
  g_auto (GVariantBuilder) results = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (XdpWpAppAudioSession) session = NULL;
  g_autoptr (XdpWpAppAudioStream) stream = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GError) stream_error = NULL;

  session = lookup_caller_session (self, invocation, session_handle);
  if (!session)
    return;
  g_object_ref (session);

  stream = dex_await_object (xdp_wp_app_audio_stream_new_future (self->core),
                             &stream_error);
  if (!stream)
    g_set_error (&error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                 "Can't create the application audio stream: %s", stream_error->message);
  else if (session->closed || xdp_wp_app_audio_stream_is_closed (stream))
    g_set_error_literal (&error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                         "The session or the stream was closed");

  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  xdp_wp_app_audio_session_add_stream (session, stream);

  g_variant_builder_add (&results, "{sv}", "pipewire-node-id",
                         g_variant_new_uint32 (xdp_wp_app_audio_stream_get_node_id (stream)));
  g_variant_builder_add (&results, "{sv}", "pipewire-serial",
                         g_variant_new_uint64 (xdp_wp_app_audio_stream_get_serial (stream)));
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &results));
}

static gboolean
handle_create_system_stream (XdpDbusImplAppAudioCapture *object,
                             GDBusMethodInvocation      *invocation,
                             const char                 *arg_session_handle,
                             GVariant                   *arg_options)
{
  create_stream (XDP_WP_APP_AUDIO_CAPTURE (object), invocation, arg_session_handle);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_create_application_stream (XdpDbusImplAppAudioCapture *object,
                                  GDBusMethodInvocation      *invocation,
                                  const char                 *arg_session_handle,
                                  GVariant                   *arg_options)
{
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                                 "Not implemented yet");
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_wp_app_audio_capture_iface_init (XdpDbusImplAppAudioCaptureIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_create_system_stream = handle_create_system_stream;
  iface->handle_create_application_stream = handle_create_application_stream;
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  wp_warning_object (user_data, "Lost the name %s", name);
}

static void
xdp_wp_app_audio_capture_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);

  switch ((XdpWpAppAudioCaptureProps) property_id)
    {
    case PROP_CORE:
      self->core = g_value_dup_object (value);
      break;

    case PROP_CONNECTION:
      self->connection = g_value_dup_object (value);
      break;
    }
}

static void
xdp_wp_app_audio_capture_constructed (GObject *object)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (xdp_wp_app_audio_capture_parent_class)->constructed (object);

  g_assert (WP_IS_CORE (self->core));
  g_assert (G_IS_DBUS_CONNECTION (self->connection));

  self->disconnect_id = xdp_connection_track_peer_disconnect (self->connection,
                                                              on_peer_disconnect, self);

  xdp_dbus_impl_app_audio_capture_set_version (XDP_DBUS_IMPL_APP_AUDIO_CAPTURE (self), 1);
  dex_dbus_interface_skeleton_set_flags (
    DEX_DBUS_INTERFACE_SKELETON (self),
    DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self), self->connection,
                                         DESKTOP_DBUS_PATH, &error))
    {
      wp_warning_object (self, "Can't export %s: %s",
                         DESKTOP_DBUS_PATH, error->message);
      return;
    }
  self->name_id = g_bus_own_name_on_connection (self->connection,
                                                APP_AUDIO_CAPTURE_DBUS_IMPL_IFACE,
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                NULL, on_name_lost, self, NULL);
}

static void
xdp_wp_app_audio_capture_dispose (GObject *object)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);

  g_clear_handle_id (&self->name_id, g_bus_unown_name);
  if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self)))
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self));
  if (self->disconnect_id)
    xdp_connection_untrack_peer_disconnect (self->connection, self->disconnect_id);
  self->disconnect_id = 0;

  close_sessions (self, NULL, TRUE);

  G_OBJECT_CLASS (xdp_wp_app_audio_capture_parent_class)->dispose (object);
}

static void
xdp_wp_app_audio_capture_finalize (GObject *object)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);

  g_clear_pointer (&self->sessions, g_hash_table_unref);
  g_clear_object (&self->connection);
  g_clear_object (&self->core);

  G_OBJECT_CLASS (xdp_wp_app_audio_capture_parent_class)->finalize (object);
}

static void
xdp_wp_app_audio_capture_class_init (XdpWpAppAudioCaptureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = xdp_wp_app_audio_capture_set_property;
  object_class->constructed = xdp_wp_app_audio_capture_constructed;
  object_class->dispose = xdp_wp_app_audio_capture_dispose;
  object_class->finalize = xdp_wp_app_audio_capture_finalize;

  props[PROP_CORE] = g_param_spec_object ("core", NULL, NULL,
                                          WP_TYPE_CORE,
                                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_CONNECTION] = g_param_spec_object ("connection", NULL, NULL,
                                                G_TYPE_DBUS_CONNECTION,
                                                G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

static void
xdp_wp_app_audio_capture_init (XdpWpAppAudioCapture *self)
{
  self->sessions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

XdpWpAppAudioCapture *
xdp_wp_app_audio_capture_new (WpCore          *core,
                              GDBusConnection *connection)
{
  return g_object_new (XDP_WP_TYPE_APP_AUDIO_CAPTURE,
                       "core", core,
                       "connection", connection,
                       NULL);
}
