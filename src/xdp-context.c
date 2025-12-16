/*
 * Copyright © 2025 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "xdp-app-info-registry.h"
#include "xdp-utils.h"
#include "xdp-dbus.h"
#include "xdp-documents.h"
#include "xdp-impl-dbus.h"
#include "xdp-method-info.h"
#include "xdp-portal-config.h"
#include "xdp-session-persistence.h"

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
#include "xdp-permissions.h"
#include "power-profile-monitor.h"
#include "print.h"
#include "proxy-resolver.h"
#include "realtime.h"
#include "registry.h"
#include "remote-desktop.h"
#include "xdp-request.h"
#include "screen-cast.h"
#include "screenshot.h"
#include "secret.h"
#include "settings.h"
#include "trash.h"
#include "usb.h"
#include "wallpaper.h"
#include "speech.h"

#include "xdp-context.h"

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

  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (XdpContext,
                     xdp_context,
                     G_TYPE_OBJECT)

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

  g_cancellable_cancel (context->cancellable);
  g_clear_object (&context->cancellable);
  g_clear_object (&context->portal_config);
  g_clear_object (&context->connection);
  g_clear_object (&context->lockdown_impl);
  g_clear_object (&context->access_impl);
  g_clear_object (&context->app_info_registry);
  g_clear_pointer (&context->exported_portals, g_hash_table_unref);

  G_OBJECT_CLASS (xdp_context_parent_class)->dispose (object);
}

static void
xdp_context_class_init (XdpContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_context_dispose;
}

static void
xdp_context_init (XdpContext *context)
{
  context->cancellable = g_cancellable_new ();
}

XdpContext *
xdp_context_new (gboolean opt_verbose)
{
  XdpContext *context = g_object_new (XDP_TYPE_CONTEXT, NULL);

  context->verbose = opt_verbose;
  context->portal_config = xdp_portal_config_new (context);
  context->app_info_registry = xdp_app_info_registry_new ();
  context->exported_portals = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free,
                                                     g_object_unref);

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
authorize_callback (GDBusInterfaceSkeleton *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  app_info = xdp_app_info_registry_ensure_for_invocation_sync (context->app_info_registry,
                                                               invocation,
                                                               NULL,
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

  if (method_needs_request (invocation))
    {
      if (!xdp_request_init_invocation (invocation, app_info, &error))
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

  if (!(flags & XDP_CONTEXT_EXPORT_FLAGS_HOST_PORTAL))
    {
      /* Host portal dbus method invocations run in the main thread without yielding
       * to the main loop. This means that any later method call of any portal will
       * see the effects of the host portal method call.
       *
       * This is important because the Registry modifies the XdpAppInfo and later
       * method calls must see the modified value.
       */

      g_dbus_interface_skeleton_set_flags (
        skeleton,
        G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

      g_signal_connect_object (skeleton, "g-authorize-method",
                               G_CALLBACK (authorize_callback),
                               context,
                               G_CONNECT_DEFAULT);
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

  xdp_usb_delete_for_sender (context, name);
  notification_delete_for_sender (context, name);
  close_requests_for_sender (name);
  close_sessions_for_sender (name);
  xdp_session_persistence_delete_transient_permissions_for_sender (name);
  xdp_app_info_registry_delete (context->app_info_registry, name);
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

  init_memory_monitor (context);
  init_power_profile_monitor (context);
  init_network_monitor (context);
  init_proxy_resolver (context);
  init_trash (context);
  init_game_mode (context);
  init_realtime (context);
  init_settings (context);
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
  init_secret (context, context->cancellable);
  init_global_shortcuts (context);
  init_dynamic_launcher (context);
  init_screen_cast (context);
  init_remote_desktop (context);
  init_clipboard (context);
  init_input_capture (context);
#if HAVE_GUDEV
  init_usb (context);
#endif
#if HAVE_SPEECH
  init_speech (context);
#endif
  init_registry (context);

  return TRUE;
}
