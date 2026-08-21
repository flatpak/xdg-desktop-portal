/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-context.h"

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <spa/utils/string.h>

#include "account.h"
#include "background.h"
#include "camera.h"
#include "clipboard.h"
#include "dynamic-launcher.h"
#include "email.h"
#include "file-chooser.h"
#include "gamemode.h"
#include "global-shortcuts.h"
#include "inhibit.h"
#include "input-capture.h"
#include "location.h"
#include "memory-monitor.h"
#include "network-monitor.h"
#include "notification.h"
#include "open-uri.h"
#include "power-profile-monitor.h"
#include "print.h"
#include "proxy-resolver.h"
#include "realtime.h"
#include "registry.h"
#include "remote-desktop.h"
#include "screen-cast.h"
#include "screenshot.h"
#include "secret.h"
#include "settings.h"
#include "trash.h"
#include "usb.h"
#include "wallpaper.h"
#include "xdp-app-info-registry.h"
#include "xdp-dbus.h"
#include "xdp-documents.h"
#include "xdp-impl-dbus.h"
#include "xdp-method-info.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-pw-keys.h"
#include "xdp-request.h"
#include "xdp-session-persistence.h"
#include "xdp-utils.h"
#include "xdp-wp.h"

enum
{
  PEER_DISCONNECT,
  N_SIGNALS,
};

static guint signals[N_SIGNALS] = { 0 };

typedef enum {
  PROP_CAMERA_PRESENT = 1,
} XdpContextProps;

static GParamSpec *props[PROP_CAMERA_PRESENT + 1] = { NULL, };

struct _XdpContext
{
  GObject parent_instance;

  gboolean verbose;

  XdpPortalConfig *portal_config;
  GDBusConnection *connection;
  XdpDbusImplLockdown *lockdown_impl;
  XdpDbusImplAccess *access_impl;
  guint peer_disconnect_handle_id;
  XdpAppInfoRegistry *app_info_registry;
  GHashTable *exported_portals; /* iface name -> GDBusInterfaceSkeleton */
  GHashTable *registered_object_paths; /* char *object_path set */
  GMutex registered_object_paths_lock;

  GFileMonitor *pw_socket_monitor;
  GCancellable *pw_socket_available;
  WpCore *wp_core;
  gulong wp_core_disconnect_handle_id;
  WpObjectManager *wp_metadata_om;
  WpMetadata *wp_metadata;
  gulong wp_metadata_changed_handle_id;
  bool camera_present;

  GCancellable *cancellable;
  GPtrArray *pending_inits; /* DexFuture */
};

G_DEFINE_FINAL_TYPE (XdpContext,
                     xdp_context,
                     G_TYPE_OBJECT);

static DexFuture *
try_connect_wp_core (XdpContext *context)
{
  return dex_future_first (xdp_wp_core_connect_sync (context->wp_core),
                           dex_cancellable_new_from_cancellable (context->pw_socket_available),
                           dex_cancellable_new_from_cancellable (context->cancellable),
                           NULL);
}

static DexFuture *
try_connect_wp_core_catch_loop (DexFuture *future,
                                gpointer   user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr (GError) error = NULL;

  if (dex_future_is_resolved (future))
    return dex_future_new_true ();

  g_assert (dex_future_is_rejected (future));

  dex_future_get_value (future, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return dex_future_new_false ();

  g_warning ("Failed to connect PipeWire core: %s", error->message);

  return try_connect_wp_core (context);
}

static DexFuture *
connect_wp_core_fiber (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  DexFuture *future;
  g_autoptr (GError) error = NULL;

  /* Returns FALSE without error set if cancelled */
  future = dex_future_catch_loop (try_connect_wp_core (context),
                                  try_connect_wp_core_catch_loop,
                                  context,
                                  NULL);
  if (!dex_await_boolean (g_steal_pointer (&future), &error))
    {
      if (error)
          g_warning ("Failed to connect PipeWire core: %s", error->message);
      else
          error = g_error_new_literal (G_IO_ERROR,
                                       G_IO_ERROR_CANCELLED,
                                       "PipeWire connect loop cancelled");

      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  g_debug ("PipeWire core connected");

  return dex_future_new_true ();
}

static void
on_wp_core_disconnected (WpCore   *core,
                         gpointer  user_data)
{
  g_assert (XDP_IS_CONTEXT (user_data));

  g_debug ("PipeWire core disconnected, reconnecting");

  /* Cancellable is integrated in the fiber function */
  dex_future_disown(dex_scheduler_spawn (NULL, 0,
                                         connect_wp_core_fiber,
                                         user_data, NULL));
}

static void
on_pw_socket_changed (GFileMonitor      *monitor,
                      GFile             *file,
                      GFile             *other_file,
                      GFileMonitorEvent  event_type,
                      gpointer           user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);

  if (event_type == G_FILE_MONITOR_EVENT_CREATED)
    {
      g_debug ("PipeWire socket available");
      g_cancellable_reset (context->pw_socket_available);

      /* Cancellable is integrated in the fiber function */
      dex_future_disown(dex_scheduler_spawn (NULL, 0,
                                             connect_wp_core_fiber,
                                             user_data, NULL));
    }

  if (event_type == G_FILE_MONITOR_EVENT_DELETED)
    {
      g_debug ("PipeWire socket unavailable");
      g_cancellable_cancel (context->pw_socket_available);
    }
}

static void
on_metadata_changed (WpMetadata *metadata,
                     guint       subject,
                     const char *key,
                     const char *type,
                     const char *value,
                     gpointer    user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);

  g_return_if_fail (key != NULL);
  g_return_if_fail (type != NULL);

  g_debug ("PipeWire metadata %s (%u) changed to value %s (%s)",
           key, subject, value, type);

  if (g_strcmp0 (XDP_PW_KEY_CAMERA_PRESENT, key) == 0)
    {
      g_return_if_fail (wp_spa_type_from_name (type) == SPA_TYPE_Bool);
      g_return_if_fail (subject == PW_ID_CORE);

      context->camera_present = spa_atob (value);
      g_object_notify_by_pspec (G_OBJECT (context), props[PROP_CAMERA_PRESENT]);
    }
}

static void
on_metadata_found (WpObjectManager *manager,
                   gpointer         object,
                   gpointer         user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  WpMetadata *metadata = object;
  g_autoptr (WpIterator) iter = NULL;
  g_auto (GValue) value = G_VALUE_INIT;

  /* The client permissions ensure that only one metadata object matches the
   * object manager interest. */
  g_return_if_fail (context->wp_metadata == NULL);
  g_return_if_fail (WP_IS_METADATA (object));

  g_debug (XDP_PW_METADATA_NAME " metadata found");

  context->wp_metadata = g_object_ref (metadata);
  context->wp_metadata_changed_handle_id = g_signal_connect (context->wp_metadata,
                                                             "changed",
                                                             G_CALLBACK (on_metadata_changed),
                                                             context);

  iter = wp_metadata_new_iterator (context->wp_metadata, PW_ID_ANY);
  for (; wp_iterator_next (iter, &value); g_value_unset (&value))
    {
      WpMetadataItem *item = g_value_get_boxed (&value);

      on_metadata_changed (metadata,
                           wp_metadata_item_get_subject (item),
                           wp_metadata_item_get_key (item),
                           wp_metadata_item_get_value_type (item),
                           wp_metadata_item_get_value (item),
                           context);
    }
}

static void
on_metadata_lost (WpObjectManager *manager,
                  gpointer         object,
                  gpointer         user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);

  /* The client permissions ensure that only one metadata object matches the
   * object manager interest. */
  g_return_if_fail (g_direct_equal (context->wp_metadata, object));

  g_debug (XDP_PW_METADATA_NAME " metadata lost");

  g_clear_signal_handler (&context->wp_metadata_changed_handle_id, context->wp_metadata);
  g_clear_object (&context->wp_metadata);

  if (context->camera_present)
    {
      context->camera_present = FALSE;
      g_object_notify_by_pspec (G_OBJECT (context), props[PROP_CAMERA_PRESENT]);
    }
}

static DexFuture *
init_pw_connection_fiber (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  WpProperties *wp_core_props = NULL;
  g_autofree char *pw_socket_path = NULL;
  g_autoptr(GFile) pw_socket = NULL;
  WpObjectInterest *interest = NULL;
  DexFuture *future = NULL;
  g_autoptr (GError) error = NULL;

  wp_core_props = wp_properties_new (PW_KEY_CLIENT_ACCESS, XDP_PW_ACCESS,
                                     XDP_PW_KEY_DAEMON, "true",
                                     "module.rt", "false",
                                     NULL);
  context->wp_core = wp_core_new (NULL, NULL, g_steal_pointer (&wp_core_props));
  context->wp_core_disconnect_handle_id =
    g_signal_connect (context->wp_core,
                      "disconnected",
                      G_CALLBACK (on_wp_core_disconnected),
                      context);

  interest = wp_object_interest_new_type (WP_TYPE_METADATA);
  wp_object_interest_add_constraint (interest,
                                     WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
                                     "metadata.name",
                                     WP_CONSTRAINT_VERB_EQUALS,
                                     g_variant_new_string (XDP_PW_METADATA_NAME));
  g_assert (wp_object_interest_validate (interest, NULL));

  context->wp_metadata_om = wp_object_manager_new ();
  wp_object_manager_add_interest_full (context->wp_metadata_om,
                                       g_steal_pointer (&interest));
  wp_object_manager_request_object_features (context->wp_metadata_om,
                                             WP_TYPE_METADATA,
                                             WP_PROXY_FEATURE_BOUND);
  g_signal_connect_object (context->wp_metadata_om,
                           "object-added",
                           G_CALLBACK (on_metadata_found),
                           context,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (context->wp_metadata_om,
                           "object-removed",
                           G_CALLBACK (on_metadata_lost),
                           context,
                           G_CONNECT_DEFAULT);
  wp_core_install_object_manager (context->wp_core, context->wp_metadata_om);

  pw_socket_path = g_strdup_printf ("%s/pipewire-0",
                                    g_get_user_runtime_dir ());
  pw_socket = g_file_new_for_path (pw_socket_path);
  context->pw_socket_monitor =
    g_file_monitor_file (pw_socket, G_FILE_MONITOR_NONE, context->cancellable, &error);
  if (context->pw_socket_monitor == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to create PipeWire socket monitor: %s", error->message);

      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  future = dex_future_first (dex_file_query_exists (pw_socket),
                             dex_cancellable_new_from_cancellable (context->cancellable),
                             NULL);
  g_signal_connect_object (context->pw_socket_monitor,
                           "changed",
                           G_CALLBACK (on_pw_socket_changed),
                           context,
                           G_CONNECT_DEFAULT);

  context->pw_socket_available = g_cancellable_new ();
  if (!dex_await_boolean (future, &error))
    {
     g_cancellable_cancel (context->pw_socket_available);

     if (error)
       {
         if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
          g_warning ("Failed to check if the PipeWire socket existed: %s", error->message);

         return dex_future_new_for_error (g_steal_pointer (&error));
       }

      return dex_future_new_false ();
    }

  return connect_wp_core_fiber (context);
}

static void
xdp_context_get_property (GObject    *object,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  XdpContext *context = XDP_CONTEXT (object);

  switch ((XdpContextProps)property_id)
    {
    case PROP_CAMERA_PRESENT:
      g_value_set_boolean (value, context->camera_present);
      break;
    }
}

static void
xdp_context_dispose (GObject *object)
{
  XdpContext *context = XDP_CONTEXT (object);

  if (context->peer_disconnect_handle_id)
    {
      g_assert (context->connection);
      xdp_connection_untrack_peer_disconnect (context->connection,
                                              context->peer_disconnect_handle_id);
      context->peer_disconnect_handle_id = 0;
    }

  g_clear_signal_handler (&context->wp_core_disconnect_handle_id, context->wp_core);

  g_debug ("Shutting down portal context");

  /* Cancel in-flight fibers and unexport all portals, then drain the main
   * context so cancelled fibers can run their cleanup (e.g. unclaiming
   * object paths) before we free the remaining resources. */
  if (context->exported_portals)
    {
      GHashTableIter iter;
      GDBusInterfaceSkeleton *skeleton;

      g_hash_table_iter_init (&iter, context->exported_portals);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &skeleton))
        {
          if (DEX_IS_DBUS_INTERFACE_SKELETON (skeleton))
            dex_dbus_interface_skeleton_cancel (DEX_DBUS_INTERFACE_SKELETON (skeleton));
          g_dbus_interface_skeleton_unexport (skeleton);
        }

      g_clear_pointer (&context->exported_portals, g_hash_table_unref);
    }

  g_cancellable_cancel (context->cancellable);
  g_clear_object (&context->cancellable);

  while (g_main_context_iteration (NULL, FALSE))
    ;

  g_clear_object (&context->portal_config);
  g_clear_object (&context->connection);
  g_clear_object (&context->lockdown_impl);
  g_clear_object (&context->access_impl);
  g_clear_object (&context->app_info_registry);

  if (context->registered_object_paths)
    {
      g_clear_pointer (&context->registered_object_paths, g_hash_table_unref);
      g_mutex_clear (&context->registered_object_paths_lock);
    }

  g_clear_object (&context->wp_metadata_om);
  g_clear_object (&context->wp_metadata);
  g_clear_object (&context->pw_socket_available);
  g_clear_object (&context->pw_socket_monitor);
  g_clear_object (&context->wp_core);

  G_OBJECT_CLASS (xdp_context_parent_class)->dispose (object);
}

static void
xdp_context_class_init (XdpContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdp_context_get_property;
  object_class->dispose = xdp_context_dispose;

  props[PROP_CAMERA_PRESENT] =
    g_param_spec_boolean ("camera-present", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);

  signals[PEER_DISCONNECT] =
    g_signal_new ("peer-disconnect",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
xdp_context_init (XdpContext *context)
{
}

XdpContext *
xdp_context_new (gboolean opt_verbose)
{
  XdpContext *context = g_object_new (XDP_TYPE_CONTEXT, NULL);

  context->verbose = opt_verbose;
  context->cancellable = g_cancellable_new ();
  context->portal_config = xdp_portal_config_new (context);
  context->app_info_registry = xdp_app_info_registry_new ();
  context->exported_portals = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free,
                                                     g_object_unref);
  context->registered_object_paths =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_mutex_init (&context->registered_object_paths_lock);

  return context;
}

gboolean
xdp_context_is_verbose (XdpContext *context)
{
  return context->verbose;
}

XdpAppInfoRegistry *
xdp_context_get_app_info_registry (XdpContext *context)
{
  return context->app_info_registry;
}

GDBusConnection *
xdp_context_get_connection (XdpContext *context)
{
  return context->connection;
}

XdpPortalConfig *
xdp_context_get_config (XdpContext *context)
{
  return context->portal_config;
}

XdpDbusImplLockdown *
xdp_context_get_lockdown_impl (XdpContext *context)
{
  return context->lockdown_impl;
}

XdpDbusImplAccess *
xdp_context_get_access_impl (XdpContext *context)
{
  return context->access_impl;
}

static gboolean
method_needs_request (GDBusMethodInvocation *invocation)
{
  const char *interface;
  const char *method;
  const XdpMethodInfo *method_info;

  interface = g_dbus_method_invocation_get_interface_name (invocation);
  method = g_dbus_method_invocation_get_method_name (invocation);

  method_info = xdp_method_info_find (interface, method);

  if (!method_info)
    g_warning ("Support for %s::%s missing in %s",
               interface, method, G_STRLOC);

  return method_info ?  method_info->uses_request : TRUE;
}

static gboolean
authorize_callback_fiber (GDBusInterfaceSkeleton *interface,
                          GDBusMethodInvocation  *invocation,
                          gpointer                user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  app_info = dex_await_object (xdp_app_info_registry_ensure_future (
      context->app_info_registry,
      invocation),
    &error);

  if (app_info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: %s", error->message);
      return FALSE;
    }

  g_object_set_data (G_OBJECT (invocation), "xdp-app-info", app_info);

  return TRUE;
}

static gboolean
authorize_callback (GDBusInterfaceSkeleton *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  future = xdp_app_info_registry_ensure_future (context->app_info_registry,
                                                invocation);
  dex_thread_wait_for (dex_ref (future), NULL);

  app_info = dex_await_object (g_steal_pointer (&future), &error);
  if (app_info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: %s", error->message);
      return FALSE;
    }

  g_object_set_data_full (G_OBJECT (invocation), "xdp-app-info",
                          g_object_ref (app_info), g_object_unref);

  if (method_needs_request (invocation))
    {
      if (!xdp_request_init_invocation (invocation, context, app_info, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return FALSE;
        }
    }

  return TRUE;
}

void
xdp_context_take_and_export_portal (XdpContext             *context,
                                    GDBusInterfaceSkeleton *skeleton_,
                                    XdpContextExportFlags   flags)
{
  g_autoptr(GDBusInterfaceSkeleton) skeleton = skeleton_;
  g_autoptr(GError) error = NULL;
  const char *name;

  g_return_if_fail (XDP_IS_CONTEXT (context));
  g_return_if_fail (G_IS_DBUS_INTERFACE_SKELETON (skeleton));

  name = g_dbus_interface_skeleton_get_info (skeleton)->name;

  if (flags & XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD)
    {
      g_dbus_interface_skeleton_set_flags (
        skeleton,
        G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
    }

  if (flags & XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER)
    {
      dex_dbus_interface_skeleton_set_flags (
        DEX_DBUS_INTERFACE_SKELETON (skeleton),
        DEX_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_FIBER);
    }

  if (!(flags & XDP_CONTEXT_EXPORT_FLAGS_SKIP_AUTH))
    {
      if (flags & XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER)
        {
          g_signal_connect_object (skeleton, "g-authorize-method",
                                   G_CALLBACK (authorize_callback_fiber),
                                   context,
                                   G_CONNECT_DEFAULT);
        }
      else
        {
          g_signal_connect_object (skeleton, "g-authorize-method",
                                   G_CALLBACK (authorize_callback),
                                   context,
                                   G_CONNECT_DEFAULT);
        }
    }

  if (g_dbus_interface_skeleton_export (skeleton,
                                        context->connection,
                                        DESKTOP_DBUS_PATH,
                                        &error))
    g_debug ("Providing portal %s", name);
  else
    g_warning ("Exporting portal failed: %s", error->message);

  g_hash_table_insert (context->exported_portals,
                       g_strdup (name),
                       g_steal_pointer (&skeleton));
}

GDBusInterfaceSkeleton *
xdp_context_get_portal (XdpContext *context,
                        const char *interface)
{
  return g_hash_table_lookup (context->exported_portals, interface);
}

static void
on_peer_disconnect (const char *name,
                    gpointer    user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);

  g_signal_emit (context, signals[PEER_DISCONNECT], 0, name);

  dex_future_disown (xdp_app_info_registry_delete_future (context->app_info_registry,
                                                          name));
}

static void
init_portal_in_fiber (XdpContext   *context,
                      DexFiberFunc  portal_init_func)
{
  g_autoptr(DexFuture) f = NULL;
  GCancellable *cancellable = context->cancellable;

  f = dex_future_first (dex_scheduler_spawn (NULL, 0,
                                             portal_init_func,
                                             context, NULL),
                        dex_cancellable_new_from_cancellable (cancellable),
                        NULL);

  if (context->pending_inits == NULL)
    context->pending_inits = g_ptr_array_new_with_free_func (dex_unref);

  g_ptr_array_add (context->pending_inits, g_steal_pointer (&f));
}

static void
await_pending_inits (XdpContext *context)
{
  g_autoptr(GPtrArray) pending_inits = g_steal_pointer (&context->pending_inits);
  g_autoptr(DexFuture) all = NULL;

  if (pending_inits == NULL)
    return;

  all = dex_future_allv ((DexFuture *const *) pending_inits->pdata,
                         pending_inits->len);

  while (dex_future_is_pending (all))
    g_main_context_iteration (g_main_context_get_thread_default (), TRUE);
}

gboolean
xdp_context_register (XdpContext       *context,
                      GDBusConnection  *connection,
                      GError          **error)
{
  XdpPortalConfig *portal_config = context->portal_config;
  XdpImplConfig *lockdown_impl_config;
  XdpImplConfig *access_impl_config;
  GQuark portal_errors G_GNUC_UNUSED;

  /* make sure errors are registered */
  portal_errors = XDG_DESKTOP_PORTAL_ERROR;

  g_set_object (&context->connection, connection);

  context->peer_disconnect_handle_id =
    xdp_connection_track_peer_disconnect (connection,
                                          on_peer_disconnect,
                                          context);

  if (!xdp_init_permission_store (connection, error))
    {
      g_prefix_error_literal (error, "No permission store: ");
      return FALSE;
    }

  if (!xdp_init_document_proxy (connection, error))
    {
      g_prefix_error_literal (error, "No document portal: ");
      return FALSE;
    }

  lockdown_impl_config = xdp_portal_config_find (portal_config, LOCKDOWN_DBUS_IMPL_IFACE);
  if (lockdown_impl_config != NULL)
    {
      context->lockdown_impl =
        xdp_dbus_impl_lockdown_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               lockdown_impl_config->dbus_name,
                                               DESKTOP_DBUS_PATH,
                                               NULL, NULL);
    }

  if (context->lockdown_impl == NULL)
    context->lockdown_impl = xdp_dbus_impl_lockdown_skeleton_new ();

  access_impl_config = xdp_portal_config_find (portal_config, ACCESS_DBUS_IMPL_IFACE);
  if (access_impl_config != NULL)
    {
      context->access_impl =
        xdp_dbus_impl_access_proxy_new_sync (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             access_impl_config->dbus_name,
                                             DESKTOP_DBUS_PATH,
                                             NULL, NULL);
    }

  if (context->access_impl)
    {
        g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (context->access_impl),
                                          G_MAXINT);
    }

  /* Cancellable is integrated in the fiber function */
  dex_future_disown (dex_scheduler_spawn (NULL, 0,
                                          init_pw_connection_fiber,
                                          context, NULL));

  init_portal_in_fiber (context, init_secret);
  init_memory_monitor (context);
  init_power_profile_monitor (context);
  init_network_monitor (context);
  init_proxy_resolver (context);
  init_trash (context);
  init_game_mode (context);
  init_realtime (context);
  init_portal_in_fiber (context, init_settings);
  init_file_chooser (context);
  init_open_uri (context);
  init_print (context);
  init_notification (context);
  init_inhibit (context);
#if HAVE_GEOCLUE
  init_location (context);
#endif
  init_camera (context);
  init_screenshot (context);
  init_background (context);
  init_wallpaper (context);
  init_account (context);
  init_email (context);
  init_global_shortcuts (context);
  init_dynamic_launcher (context);
  init_screen_cast (context);
  init_remote_desktop (context);
  init_clipboard (context);
  init_input_capture (context);
#if HAVE_GUDEV
  init_usb (context);
#endif
  init_registry (context);

  await_pending_inits (context);

  return TRUE;
}

gboolean
xdp_context_claim_object_path (XdpContext *context,
                               const char *object_path)
{
  G_MUTEX_AUTO_LOCK (&context->registered_object_paths_lock, locker);

  if (g_hash_table_contains (context->registered_object_paths, object_path))
    return FALSE;

  g_hash_table_add (context->registered_object_paths,
                    g_strdup (object_path));
  return TRUE;
}

void
xdp_context_unclaim_object_path (XdpContext *context,
                                 const char *object_path)
{
  G_MUTEX_AUTO_LOCK (&context->registered_object_paths_lock, locker);

  g_hash_table_remove (context->registered_object_paths, object_path);
}
