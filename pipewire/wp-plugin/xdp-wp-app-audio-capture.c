/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#include "xdp-wp-app-audio-capture.h"

#include <libdex.h>
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/string.h>

#include "xdp-app-info.h"
#include "xdp-screen-cast.h"
#include "xdp-utils.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-app-audio-capture");

#define LINK_GROUP_PREFIX "xdp-app-audio-"
#define OUTPUT_MEDIA_CLASS "Stream/Output/Audio"
#define INPUT_MEDIA_CLASS "Stream/Input/Audio"
#define START_TIMEOUT_MS 1000
#define POSITIONS_7_1 "[ FL, FR, FC, LFE, RL, RR, SL, SR ]"

#define XDP_WP_TYPE_CLIENT_IDENTITY_CACHE (xdp_wp_client_identity_cache_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpClientIdentityCache, xdp_wp_client_identity_cache,
                      XDP_WP, CLIENT_IDENTITY_CACHE, GObject)

#define XDP_WP_TYPE_APP_AUDIO_LOOPBACK (xdp_wp_app_audio_loopback_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioLoopback, xdp_wp_app_audio_loopback, XDP_WP, APP_AUDIO_LOOPBACK,
                      GObject)

#define XDP_WP_TYPE_APP_AUDIO_STREAM (xdp_wp_app_audio_stream_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioStream, xdp_wp_app_audio_stream, XDP_WP, APP_AUDIO_STREAM,
                      GObject)

#define XDP_WP_TYPE_APP_AUDIO_GRAPH (xdp_wp_app_audio_graph_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioGraph, xdp_wp_app_audio_graph, XDP_WP, APP_AUDIO_GRAPH,
                      GObject)

#define XDP_WP_TYPE_APP_AUDIO_SESSION (xdp_wp_app_audio_session_get_type ())
G_DECLARE_FINAL_TYPE (XdpWpAppAudioSession, xdp_wp_app_audio_session, XDP_WP, APP_AUDIO_SESSION,
                      XdpDbusImplSessionSkeleton)

struct _XdpWpClientIdentityCache
{
  GObject parent_instance;

  WpObjectManager *clients;
  GHashTable *app_ids; /* guint32 client id -> char *app_id */
};

G_DEFINE_FINAL_TYPE (XdpWpClientIdentityCache, xdp_wp_client_identity_cache, G_TYPE_OBJECT)

enum
{
  IDENTITY_CACHE_CHANGED,
  N_IDENTITY_CACHE_SIGNALS,
};

static guint identity_cache_signals[N_IDENTITY_CACHE_SIGNALS];

static char *
dup_pid_app_id (pid_t pid)
{
  g_autoptr (XdpAppInfo) app_info = NULL;
  const char *app_id;

  if (pid <= 0)
    return NULL;

  app_info = xdp_app_info_new (NULL, pid, -1, NULL);
  app_id = app_info ? xdp_app_info_get_id (app_info) : NULL;
  return app_id && *app_id ? g_strdup (app_id) : NULL;
}

/* TODO: This whole function should ideally be gone once there's appd
 * integration in pipewire to always give us a reliable app id */
static void
identify_client (XdpWpClientIdentityCache *self,
                 WpClient                 *client)
{
  WpPipewireObject *object = WP_PIPEWIRE_OBJECT (client);
  gpointer id = GUINT_TO_POINTER (wp_proxy_get_bound_id (WP_PROXY (client)));
  const char *client_app_id =
    wp_pipewire_object_get_property (object, "pipewire.access.portal.app_id");
  const char *snap_id = wp_pipewire_object_get_property (object, "pipewire.snap.id");
  gboolean is_pulse = g_strcmp0 (wp_pipewire_object_get_property (object, PW_KEY_CLIENT_API),
                                 "pipewire-pulse") == 0;
  g_autofree char *app_id = NULL;

  if (!client_app_id || !*client_app_id)
    client_app_id = wp_pipewire_object_get_property (object, PW_KEY_SEC_APP_ID);
  if (client_app_id && *client_app_id)
    {
      app_id = g_strdup (client_app_id);
    }
  else if (snap_id && *snap_id)
    {
      app_id = g_strconcat ("snap.", snap_id, NULL);
    }
  else
    {
      const char *pid_str =
        wp_pipewire_object_get_property (object, is_pulse ? PW_KEY_APP_PROCESS_ID
                                                          : PW_KEY_SEC_PID);
      gint32 pid;

      if (pid_str && spa_atoi32 (pid_str, &pid, 10))
        app_id = dup_pid_app_id (pid);
    }

  g_hash_table_insert (self->app_ids, id, g_steal_pointer (&app_id));
  g_signal_emit (self, identity_cache_signals[IDENTITY_CACHE_CHANGED], 0);
}

static void
on_client_changed (XdpWpClientIdentityCache *self,
                   GParamSpec               *pspec,
                   WpClient                 *client)
{
  identify_client (self, client);
}

static void
on_client_added (XdpWpClientIdentityCache *self,
                 WpClient                 *client)
{
  identify_client (self, client);
  g_signal_connect_object (client, "notify::properties",
                           G_CALLBACK (on_client_changed), self, G_CONNECT_SWAPPED);
}

static void
on_client_removed (XdpWpClientIdentityCache *self,
                   WpClient                 *client)
{
  g_hash_table_remove (self->app_ids,
                       GUINT_TO_POINTER (wp_proxy_get_bound_id (WP_PROXY (client))));
}

static const char *
xdp_wp_client_identity_cache_lookup (XdpWpClientIdentityCache *self,
                                     guint32                   client_id)
{
  return g_hash_table_lookup (self->app_ids, GUINT_TO_POINTER (client_id));
}

static void
xdp_wp_client_identity_cache_dispose (GObject *object)
{
  XdpWpClientIdentityCache *self = XDP_WP_CLIENT_IDENTITY_CACHE (object);

  g_clear_object (&self->clients);
  g_hash_table_remove_all (self->app_ids);

  G_OBJECT_CLASS (xdp_wp_client_identity_cache_parent_class)->dispose (object);
}

static void
xdp_wp_client_identity_cache_finalize (GObject *object)
{
  XdpWpClientIdentityCache *self = XDP_WP_CLIENT_IDENTITY_CACHE (object);

  g_clear_pointer (&self->app_ids, g_hash_table_unref);

  G_OBJECT_CLASS (xdp_wp_client_identity_cache_parent_class)->finalize (object);
}

static void
xdp_wp_client_identity_cache_class_init (XdpWpClientIdentityCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_wp_client_identity_cache_dispose;
  object_class->finalize = xdp_wp_client_identity_cache_finalize;

  identity_cache_signals[IDENTITY_CACHE_CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
xdp_wp_client_identity_cache_init (XdpWpClientIdentityCache *self)
{
  self->app_ids = g_hash_table_new_full (NULL, NULL, NULL, g_free);
}

static XdpWpClientIdentityCache *
xdp_wp_client_identity_cache_new (WpCore *core)
{
  XdpWpClientIdentityCache *self = g_object_new (XDP_WP_TYPE_CLIENT_IDENTITY_CACHE, NULL);

  self->clients = wp_object_manager_new ();
  wp_object_manager_add_interest (self->clients, WP_TYPE_CLIENT, NULL);
  wp_object_manager_request_object_features (self->clients, WP_TYPE_CLIENT,
                                             WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  g_signal_connect_object (self->clients, "object-added",
                           G_CALLBACK (on_client_added), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->clients, "object-removed",
                           G_CALLBACK (on_client_removed), self, G_CONNECT_SWAPPED);
  wp_core_install_object_manager (core, self->clients);
  return self;
}

struct _XdpWpAppAudioLoopback
{
  GObject parent_instance;

  WpCore *core;
  WpImplModule *module;
  WpObjectManager *nodes;
  WpNode *capture;
  WpNode *playback;
  WpObjectManager *readers;
  GSource *closed_source;
};

G_DEFINE_FINAL_TYPE (XdpWpAppAudioLoopback, xdp_wp_app_audio_loopback, G_TYPE_OBJECT)

enum
{
  LOOPBACK_CLOSED,
  LOOPBACK_CHANGED,
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

static void
on_readers_changed (XdpWpAppAudioLoopback *self)
{
  g_signal_emit (self, loopback_signals[LOOPBACK_CHANGED], 0);
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

  self->readers = wp_object_manager_new ();
  wp_object_manager_add_interest (self->readers, WP_TYPE_LINK,
                                  WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
                                  PW_KEY_LINK_OUTPUT_NODE, "=u",
                                  wp_proxy_get_bound_id (WP_PROXY (self->playback)),
                                  NULL);
  g_signal_connect_object (self->readers, "objects-changed",
                           G_CALLBACK (on_readers_changed), self, G_CONNECT_SWAPPED);
  wp_core_install_object_manager (self->core, self->readers);
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

static WpNode *
xdp_wp_app_audio_loopback_get_capture_node (XdpWpAppAudioLoopback *self)
{
  return self->capture;
}

static gboolean
xdp_wp_app_audio_loopback_is_closed (XdpWpAppAudioLoopback *self)
{
  return !self->playback;
}

static gboolean
xdp_wp_app_audio_loopback_has_reader (XdpWpAppAudioLoopback *self)
{
  return wp_object_manager_get_n_objects (self->readers) > 0;
}

static void
xdp_wp_app_audio_loopback_dispose (GObject *object)
{
  XdpWpAppAudioLoopback *self = XDP_WP_APP_AUDIO_LOOPBACK (object);

  if (self->closed_source)
    g_source_destroy (self->closed_source);
  g_clear_pointer (&self->closed_source, g_source_unref);
  g_clear_object (&self->readers);
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
  loopback_signals[LOOPBACK_CHANGED] =
    g_signal_new ("changed",
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
  XdpScreenCastAudioType type;
  char *target_app; /* NULL for System, or an application with no app id */
  char *excluded_app; /* the session's application with exclude_self, else NULL */
  GHashTable *links; /* application stream node id -> WpSessionItem *link */
};

G_DEFINE_FINAL_TYPE (XdpWpAppAudioStream, xdp_wp_app_audio_stream, G_TYPE_OBJECT)

enum
{
  STREAM_CLOSED,
  STREAM_CHANGED,
  N_STREAM_SIGNALS,
};

static guint stream_signals[N_STREAM_SIGNALS];

static void
emit_changed (XdpWpAppAudioStream *self)
{
  g_signal_emit (self, stream_signals[STREAM_CHANGED], 0);
}

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
  g_signal_connect_object (self->loopback, "changed",
                           G_CALLBACK (emit_changed), self, G_CONNECT_SWAPPED);
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

static gboolean
xdp_wp_app_audio_stream_carries (XdpWpAppAudioStream *self,
                                 const char          *app_id)
{
  if (app_id && g_strcmp0 (self->excluded_app, app_id) == 0)
    return FALSE;

  if (self->type == XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM)
    return TRUE;
  return app_id && g_strcmp0 (self->target_app, app_id) == 0;
}

static void
remove_link (gpointer data)
{
  WpSessionItem *link = data;

  wp_object_deactivate (WP_OBJECT (link), WP_SESSION_ITEM_FEATURE_ACTIVE);
  wp_session_item_remove (link);
  g_object_unref (link);
}

static gboolean
is_value (gpointer key,
          gpointer value,
          gpointer user_data)
{
  return value == user_data;
}

static void
on_link_activated (GObject      *link,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  GWeakRef *weak_self = user_data;
  g_autoptr (XdpWpAppAudioStream) self = g_weak_ref_get (weak_self);
  g_autoptr (GError) error = NULL;

  g_weak_ref_clear (weak_self);
  g_free (weak_self);

  if (wp_object_activate_finish (WP_OBJECT (link), result, &error) || !self)
    return;

  wp_info_object (link, "Can't link a stream into a loopback's capture node: %s",
                  error->message);
  g_hash_table_foreach_remove (self->links, is_value, link);
}

static void
link_stream (XdpWpAppAudioStream *self,
             guint32              id,
             WpSiLinkable        *app_stream,
             WpSiLinkable        *capture)
{
  /* TODO: Move to WpStream once we move to WirePlumber 0.6
   * https://gitlab.freedesktop.org/pipewire/wireplumber/-/merge_requests/895 */
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (app_stream));
  g_autoptr (WpSessionItem) link = wp_session_item_make (core, "si-standard-link");
  WpProperties *props;
  GWeakRef *weak_self;

  if (!link)
    return;

  props = wp_properties_new ("out.item.port.context", "output",
                             "in.item.port.context", "input",
                             NULL);
  wp_properties_setf (props, "out.item", "%p", app_stream);
  wp_properties_setf (props, "in.item", "%p", capture);
  if (!wp_session_item_configure (link, props))
    {
      wp_info_object (self, "Can't link application stream %u into a loopback's capture node", id);
      return;
    }

  wp_session_item_register (g_object_ref (link));
  g_hash_table_insert (self->links, GUINT_TO_POINTER (id), g_object_ref (link));
  weak_self = g_new0 (GWeakRef, 1);
  g_weak_ref_init (weak_self, self);
  wp_object_activate (WP_OBJECT (link), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
                      on_link_activated, weak_self);
}

static void
xdp_wp_app_audio_stream_link (XdpWpAppAudioStream *self,
                              WpSiLinkable        *capture,
                              GHashTable          *wanted)
{
  GHashTableIter iter;
  gpointer id, value;

  /* Keep only the wanted links, from their application stream's current session item */
  g_hash_table_iter_init (&iter, self->links);
  while (g_hash_table_iter_next (&iter, &id, &value))
    {
      const char *out_item = wp_session_item_get_property (value, "out.item.id");
      WpSiLinkable *app_stream = capture ? g_hash_table_lookup (wanted, id) : NULL;
      guint32 out_id;

      if (!app_stream || !out_item || !spa_atou32 (out_item, &out_id, 10) ||
          out_id != wp_object_get_id (WP_OBJECT (app_stream)))
        g_hash_table_iter_remove (&iter);
    }

  if (!capture)
    return;

  g_hash_table_iter_init (&iter, wanted);
  while (g_hash_table_iter_next (&iter, &id, &value))
    if (!g_hash_table_contains (self->links, id))
      link_stream (self, GPOINTER_TO_UINT (id), value, capture);
}

static void
xdp_wp_app_audio_stream_forget_link (XdpWpAppAudioStream *self,
                                     guint32              node_id)
{
  g_hash_table_remove (self->links, GUINT_TO_POINTER (node_id));
}

static void
xdp_wp_app_audio_stream_dispose (GObject *object)
{
  XdpWpAppAudioStream *self = XDP_WP_APP_AUDIO_STREAM (object);

  g_hash_table_remove_all (self->links);
  g_clear_object (&self->loopback);

  G_OBJECT_CLASS (xdp_wp_app_audio_stream_parent_class)->dispose (object);
}

static void
xdp_wp_app_audio_stream_finalize (GObject *object)
{
  XdpWpAppAudioStream *self = XDP_WP_APP_AUDIO_STREAM (object);

  g_clear_pointer (&self->links, g_hash_table_unref);
  g_clear_pointer (&self->excluded_app, g_free);
  g_clear_pointer (&self->target_app, g_free);

  G_OBJECT_CLASS (xdp_wp_app_audio_stream_parent_class)->finalize (object);
}

static void
xdp_wp_app_audio_stream_class_init (XdpWpAppAudioStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_wp_app_audio_stream_dispose;
  object_class->finalize = xdp_wp_app_audio_stream_finalize;

  stream_signals[STREAM_CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  stream_signals[STREAM_CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
xdp_wp_app_audio_stream_init (XdpWpAppAudioStream *self)
{
  self->links = g_hash_table_new_full (NULL, NULL, NULL, remove_link);
}

struct _XdpWpAppAudioGraph
{
  GObject parent_instance;

  WpCore *core;
  XdpWpClientIdentityCache *identities;
  WpObjectManager *nodes;
  WpObjectManager *linkables;
  WpEventHook *relink_hook;
  GSource *relink_source;
  GPtrArray *streams; /* XdpWpAppAudioStream, not owned */
};

G_DEFINE_FINAL_TYPE (XdpWpAppAudioGraph, xdp_wp_app_audio_graph, G_TYPE_OBJECT)

static guint32
get_client_id (WpNode *node)
{
  const char *str = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (node), PW_KEY_CLIENT_ID);
  guint32 id;

  return str && spa_atou32 (str, &id, 10) ? id : SPA_ID_INVALID;
}

static gboolean
is_app_stream (WpNode *node)
{
  WpPipewireObject *object = WP_PIPEWIRE_OBJECT (node);

  return g_strcmp0 (wp_pipewire_object_get_property (object, PW_KEY_MEDIA_CLASS),
                    OUTPUT_MEDIA_CLASS) == 0 &&
         !spa_atob (wp_pipewire_object_get_property (object, PW_KEY_STREAM_MONITOR)) &&
         !wp_pipewire_object_get_property (object, PW_KEY_NODE_LINK_GROUP);
}

static GHashTable *
carried_app_streams (XdpWpAppAudioGraph  *self,
                     XdpWpAppAudioStream *stream,
                     GHashTable          *app_streams)
{
  GHashTable *carried = g_hash_table_new (NULL, NULL);
  GHashTableIter iter;
  gpointer id, linkable;

  g_hash_table_iter_init (&iter, app_streams);
  while (g_hash_table_iter_next (&iter, &id, &linkable))
    {
      g_autoptr (WpNode) node = wp_session_item_get_associated_proxy (linkable, WP_TYPE_NODE);
      guint32 client_id = node ? get_client_id (node) : SPA_ID_INVALID;
      const char *app_id = xdp_wp_client_identity_cache_lookup (self->identities, client_id);

      if (xdp_wp_app_audio_stream_carries (stream, app_id))
        g_hash_table_insert (carried, id, linkable);
    }
  return carried;
}

static WpSiLinkable *
lookup_capture (GHashTable          *linkables,
                XdpWpAppAudioStream *stream)
{
  WpNode *capture;

  if (xdp_wp_app_audio_stream_is_closed (stream) ||
      !xdp_wp_app_audio_loopback_has_reader (stream->loopback))
    return NULL;
  capture = xdp_wp_app_audio_loopback_get_capture_node (stream->loopback);
  return g_hash_table_lookup (linkables,
                              GUINT_TO_POINTER (wp_proxy_get_bound_id (WP_PROXY (capture))));
}

/* Every change recomputes the wanted links and applies only the difference:
 * - wanted: the application streams (Stream/Output/Audio, not a monitor, not in a link
 *   group) that each loopback carries, into its capture node if it has a reader;
 * - recomputed when nodes come or go, a loopback gains or loses a reader, a loopback is created,
 *   or WirePlumber relinks an application stream;
 * - a link that fails is dropped and retried on the next change; closing a loopback removes
 *   its links. */
static gboolean
relink (gpointer user_data)
{
  XdpWpAppAudioGraph *self = user_data;
  g_autoptr (GHashTable) linkables = g_hash_table_new (NULL, NULL);
  g_autoptr (GHashTable) app_streams = g_hash_table_new (NULL, NULL);
  g_autoptr (WpIterator) it = NULL;
  g_auto (GValue) value = G_VALUE_INIT;

  g_clear_pointer (&self->relink_source, g_source_unref);

  it = wp_object_manager_new_iterator (self->linkables);
  for (; wp_iterator_next (it, &value); g_value_unset (&value))
    {
      WpSessionItem *linkable = g_value_get_object (&value);
      const char *str = wp_session_item_get_property (linkable, "node.id");
      guint32 id;

      if (str && spa_atou32 (str, &id, 10))
        g_hash_table_insert (linkables, GUINT_TO_POINTER (id), linkable);
    }
  g_clear_pointer (&it, wp_iterator_unref);

  it = wp_object_manager_new_filtered_iterator (self->nodes, WP_TYPE_NODE,
                                                WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                                PW_KEY_MEDIA_CLASS, "=s", OUTPUT_MEDIA_CLASS,
                                                NULL);
  for (; wp_iterator_next (it, &value); g_value_unset (&value))
    {
      WpNode *node = g_value_get_object (&value);
      gpointer id = GUINT_TO_POINTER (wp_proxy_get_bound_id (WP_PROXY (node)));
      gpointer linkable = g_hash_table_lookup (linkables, id);

      if (is_app_stream (node) && linkable)
        g_hash_table_insert (app_streams, id, linkable);
    }

  for (guint i = 0; i < self->streams->len; i++)
    {
      XdpWpAppAudioStream *stream = g_ptr_array_index (self->streams, i);
      g_autoptr (GHashTable) carried = carried_app_streams (self, stream, app_streams);

      xdp_wp_app_audio_stream_link (stream, lookup_capture (linkables, stream), carried);
    }
  return G_SOURCE_REMOVE;
}

static void
schedule_relink (XdpWpAppAudioGraph *self)
{
  if (!self->relink_source)
    wp_core_idle_add (self->core, &self->relink_source, relink, self, NULL);
}

static void
on_target_linked (WpEvent            *event,
                  XdpWpAppAudioGraph *self)
{
  const GValue *target = wp_event_get_data (event, "target");
  g_autoptr (GObject) subject = wp_event_get_subject (event);
  guint32 id;

  if (!target || !G_VALUE_HOLDS_OBJECT (target) || !g_value_get_object (target) ||
      !WP_IS_SESSION_ITEM (subject))
    return;

  id = wp_session_item_get_associated_proxy_id (WP_SESSION_ITEM (subject), WP_TYPE_NODE);
  for (guint i = 0; i < self->streams->len; i++)
    xdp_wp_app_audio_stream_forget_link (g_ptr_array_index (self->streams, i), id);
  schedule_relink (self);
}

static void
on_stream_finalized (gpointer  user_data,
                     GObject  *stream)
{
  XdpWpAppAudioGraph *self = user_data;

  g_ptr_array_remove_fast (self->streams, stream);
}

static void
xdp_wp_app_audio_graph_add_stream (XdpWpAppAudioGraph  *self,
                                   XdpWpAppAudioStream *stream)
{
  g_ptr_array_add (self->streams, stream);
  g_object_weak_ref (G_OBJECT (stream), on_stream_finalized, self);
  g_signal_connect_object (stream, "changed",
                           G_CALLBACK (schedule_relink), self, G_CONNECT_SWAPPED);
  schedule_relink (self);
}

static WpObjectManager *
watch_objects (XdpWpAppAudioGraph *self,
               GType               type)
{
  WpObjectManager *manager = wp_object_manager_new ();

  wp_object_manager_add_interest (manager, type, NULL);
  if (g_type_is_a (type, WP_TYPE_GLOBAL_PROXY))
    wp_object_manager_request_object_features (manager, type, WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  g_signal_connect_object (manager, "objects-changed",
                           G_CALLBACK (schedule_relink), self, G_CONNECT_SWAPPED);
  wp_core_install_object_manager (self->core, manager);
  return manager;
}

static void
xdp_wp_app_audio_graph_dispose (GObject *object)
{
  XdpWpAppAudioGraph *self = XDP_WP_APP_AUDIO_GRAPH (object);

  for (guint i = 0; i < self->streams->len; i++)
    g_object_weak_unref (g_ptr_array_index (self->streams, i), on_stream_finalized, self);
  g_ptr_array_set_size (self->streams, 0);

  if (self->relink_source)
    g_source_destroy (self->relink_source);
  g_clear_pointer (&self->relink_source, g_source_unref);

  if (self->relink_hook)
    {
      g_autoptr (WpEventDispatcher) dispatcher = wp_event_dispatcher_get_instance (self->core);

      wp_event_dispatcher_unregister_hook (dispatcher, self->relink_hook);
      g_clear_object (&self->relink_hook);
    }

  g_clear_object (&self->linkables);
  g_clear_object (&self->nodes);
  g_clear_object (&self->identities);
  g_clear_object (&self->core);

  G_OBJECT_CLASS (xdp_wp_app_audio_graph_parent_class)->dispose (object);
}

static void
xdp_wp_app_audio_graph_finalize (GObject *object)
{
  XdpWpAppAudioGraph *self = XDP_WP_APP_AUDIO_GRAPH (object);

  g_clear_pointer (&self->streams, g_ptr_array_unref);

  G_OBJECT_CLASS (xdp_wp_app_audio_graph_parent_class)->finalize (object);
}

static void
xdp_wp_app_audio_graph_class_init (XdpWpAppAudioGraphClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_wp_app_audio_graph_dispose;
  object_class->finalize = xdp_wp_app_audio_graph_finalize;
}

static void
xdp_wp_app_audio_graph_init (XdpWpAppAudioGraph *self)
{
  self->streams = g_ptr_array_new ();
}

static XdpWpAppAudioGraph *
xdp_wp_app_audio_graph_new (WpCore *core)
{
  XdpWpAppAudioGraph *self = g_object_new (XDP_WP_TYPE_APP_AUDIO_GRAPH, NULL);
  g_autoptr (WpEventDispatcher) dispatcher = wp_event_dispatcher_get_instance (core);

  self->core = g_object_ref (core);
  self->identities = xdp_wp_client_identity_cache_new (core);
  g_signal_connect_object (self->identities, "changed",
                           G_CALLBACK (schedule_relink), self, G_CONNECT_SWAPPED);
  self->nodes = watch_objects (self, WP_TYPE_NODE);
  self->linkables = watch_objects (self, WP_TYPE_SI_LINKABLE);

  /* Linking an application stream to its sink reconfigures its ports and drops our links, so
   * link it again right after WirePlumber's "linking/link-target" does that */
  self->relink_hook =
    wp_simple_event_hook_new ("m-xdp-app-audio-capture/select-target",
                              (const char *[]) { NULL },
                              (const char *[]) { "linking/link-target", NULL },
                              g_cclosure_new_object (G_CALLBACK (on_target_linked),
                                                     G_OBJECT (self)));
  wp_interest_event_hook_add_interest (WP_INTEREST_EVENT_HOOK (self->relink_hook),
                                       WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                       "event.type", "=s", "select-target",
                                       NULL);
  wp_event_dispatcher_register_hook (dispatcher, self->relink_hook);
  return self;
}

struct _XdpWpAppAudioSession
{
  XdpDbusImplSessionSkeleton parent_instance;

  char *handle;
  char *owner;
  char *app_id;
  gboolean exclude_self;
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
  g_clear_pointer (&self->app_id, g_free);
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
                              const char *owner,
                              const char *app_id,
                              gboolean    exclude_self)
{
  XdpWpAppAudioSession *self = g_object_new (XDP_WP_TYPE_APP_AUDIO_SESSION, NULL);

  self->handle = g_strdup (handle);
  self->owner = g_strdup (owner);
  self->app_id = g_strdup (app_id);
  self->exclude_self = exclude_self;
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
  XdpWpAppAudioGraph *graph;
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

static gboolean
validate_pid (const char  *key,
              GVariant    *value,
              GVariant    *options,
              gpointer     user_data,
              GError     **error)
{
  if (g_variant_get_uint32 (value) > 0)
    return TRUE;

  g_set_error_literal (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "Invalid pid 0");
  return FALSE;
}

static const XdpOptionKey session_options[] = {
  { "exclude_self", G_VARIANT_TYPE_BOOLEAN, NULL },
};

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
  g_auto (GVariantBuilder) options_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GVariant) options = NULL;
  g_autoptr (XdpWpAppAudioSession) session = NULL;
  gboolean exclude_self = FALSE;
  g_autoptr (GError) error = NULL;

  if (!xdp_filter_options (arg_options, &options_builder, session_options,
                           G_N_ELEMENTS (session_options), NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  g_variant_lookup (options, "exclude_self", "b", &exclude_self);
  if (exclude_self && !*arg_app_id)
    {
      g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                     "exclude_self needs an app_id");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (g_hash_table_contains (self->sessions, arg_handle))
    {
      g_dbus_method_invocation_return_error (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_EXISTS,
                                             "A session exists at %s", arg_handle);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = xdp_wp_app_audio_session_new (arg_handle,
                                          g_dbus_method_invocation_get_sender (invocation),
                                          arg_app_id, exclude_self);
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
create_stream (XdpWpAppAudioCapture   *self,
               GDBusMethodInvocation  *invocation,
               const char             *session_handle,
               XdpScreenCastAudioType  type,
               const char             *target_app)
{
  g_auto (GVariantBuilder) results = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (XdpWpAppAudioSession) session = NULL;
  g_autoptr (XdpWpAppAudioStream) stream = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GError) stream_error = NULL;

  if (!self->graph)
    {
      g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                                     "Shutting down");
      return;
    }

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
  else if (!self->graph) /* disposed while waiting */
    g_set_error_literal (&error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                         "Shutting down");

  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  stream->type = type;
  stream->target_app = g_strdup (target_app);
  if (session->exclude_self)
    stream->excluded_app = g_strdup (session->app_id);
  xdp_wp_app_audio_session_add_stream (session, stream);
  xdp_wp_app_audio_graph_add_stream (self->graph, stream);

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
  create_stream (XDP_WP_APP_AUDIO_CAPTURE (object), invocation, arg_session_handle,
                 XDP_SCREEN_CAST_AUDIO_TYPE_SYSTEM, NULL);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static const XdpOptionKey application_stream_options[] = {
  { "pid", G_VARIANT_TYPE_UINT32, validate_pid },
};

static gboolean
handle_create_application_stream (XdpDbusImplAppAudioCapture *object,
                                  GDBusMethodInvocation      *invocation,
                                  const char                 *arg_session_handle,
                                  GVariant                   *arg_options)
{
  g_auto (GVariantBuilder) options_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GVariant) options = NULL;
  g_autofree char *target_app = NULL;
  g_autoptr (GError) error = NULL;
  guint32 pid;

  if (!xdp_filter_options (arg_options, &options_builder, application_stream_options,
                           G_N_ELEMENTS (application_stream_options), NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  if (!g_variant_lookup (options, "pid", "u", &pid))
    {
      g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                     "An application needs a pid");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  target_app = dup_pid_app_id (pid);
  create_stream (XDP_WP_APP_AUDIO_CAPTURE (object), invocation, arg_session_handle,
                 XDP_SCREEN_CAST_AUDIO_TYPE_APPLICATION, target_app);
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

  self->graph = xdp_wp_app_audio_graph_new (self->core);
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
  g_clear_object (&self->graph);

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
