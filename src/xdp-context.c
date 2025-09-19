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
#include "xdp-call.h"
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

#include "xdp-context.h"

struct _XdpContext
{
  GObject parent_instance;

  gboolean verbose;

  XdpPortalConfig *portal_config;
  GDBusConnection *connection;
  XdpDbusImplLockdown *lockdown_impl;
  guint peer_disconnect_handle_id;
  XdpAppInfoRegistry *app_info_registry;
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

  g_clear_object (&context->portal_config);
  g_clear_object (&context->connection);
  g_clear_object (&context->lockdown_impl);
  g_clear_object (&context->app_info_registry);

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
}

XdpContext *
xdp_context_new (gboolean opt_verbose)
{
  XdpContext *context = g_object_new (XDP_TYPE_CONTEXT, NULL);

  context->verbose = opt_verbose;
  context->portal_config = xdp_portal_config_new (context);
  context->app_info_registry = xdp_app_info_registry_new ();

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
  else
    xdp_call_init_invocation (invocation, app_info);

  return TRUE;
}

static void
export_portal_implementation (XdpContext             *context,
                              GDBusInterfaceSkeleton *skeleton)
{
  g_autoptr(GError) error = NULL;

  if (skeleton == NULL)
    {
      g_warning ("No skeleton to export");
      return;
    }

  g_dbus_interface_skeleton_set_flags (skeleton,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (skeleton, "g-authorize-method",
                    G_CALLBACK (authorize_callback),
                    context);

  if (!g_dbus_interface_skeleton_export (skeleton,
                                         context->connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Error: %s", error->message);
      return;
    }

  g_debug ("providing portal %s", g_dbus_interface_skeleton_get_info (skeleton)->name);
}

static void
export_host_portal_implementation (XdpContext             *context,
                                   GDBusInterfaceSkeleton *skeleton)
{
  /* Host portal dbus method invocations run in the main thread without yielding
   * to the main loop. This means that any later method call of any portal will
   * see the effects of the host portal method call.
   *
   * This is important because the Registry modifies the XdpAppInfo and later
   * method calls must see the modified value.
   */

  g_autoptr(GError) error = NULL;

  if (skeleton == NULL)
    {
      g_warning ("No skeleton to export");
      return;
    }

  g_dbus_interface_skeleton_set_flags (skeleton,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_NONE);

  if (!g_dbus_interface_skeleton_export (skeleton,
                                         context->connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Error: %s", error->message);
      return;
    }

  g_debug ("providing portal %s", g_dbus_interface_skeleton_get_info (skeleton)->name);
}

static void
on_peer_disconnect (const char *name,
                    gpointer    user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);

  xdp_app_info_registry_delete (context->app_info_registry, name);
  close_requests_for_sender (name);
  close_sessions_for_sender (name);
  xdp_session_persistence_delete_transient_permissions_for_sender (name);
  xdp_usb_delete_for_sender (name);
}

gboolean
xdp_context_register (XdpContext       *context,
                      GDBusConnection  *connection,
                      GError          **error)
{
  XdpPortalConfig *portal_config = context->portal_config;
  XdpImplConfig *impl_config;
  XdpImplConfig *lockdown_impl_config;
  XdpImplConfig *access_impl_config;
  GPtrArray *impl_configs;
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

  lockdown_impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Lockdown");
  if (lockdown_impl_config != NULL)
    {
      context->lockdown_impl =
        xdp_dbus_impl_lockdown_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               lockdown_impl_config->dbus_name,
                                               DESKTOP_PORTAL_OBJECT_PATH,
                                               NULL, NULL);
    }

  if (context->lockdown_impl == NULL)
    context->lockdown_impl = xdp_dbus_impl_lockdown_skeleton_new ();

  export_portal_implementation (context, memory_monitor_create (connection));
  export_portal_implementation (context, power_profile_monitor_create (connection));
  export_portal_implementation (context, network_monitor_create (connection));
  export_portal_implementation (context, proxy_resolver_create (connection));
  export_portal_implementation (context, trash_create (connection));
  export_portal_implementation (context, game_mode_create (connection));
  export_portal_implementation (context, realtime_create (connection));

  impl_configs = xdp_portal_config_find_all (portal_config, "org.freedesktop.impl.portal.Settings");
  if (impl_configs->len > 0)
    export_portal_implementation (context, settings_create (connection, impl_configs));
  g_ptr_array_free (impl_configs, TRUE);

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.FileChooser");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  file_chooser_create (connection, impl_config->dbus_name,
                                                       context->lockdown_impl));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.AppChooser");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  open_uri_create (connection, impl_config->dbus_name,
                                                   context->lockdown_impl));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Print");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  print_create (connection, impl_config->dbus_name,
                                                context->lockdown_impl));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Notification");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  notification_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Inhibit");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  inhibit_create (connection, impl_config->dbus_name));

  access_impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Access");
  if (access_impl_config != NULL)
    {
#if HAVE_GEOCLUE
      export_portal_implementation (context,
                                    location_create (connection,
                                                     access_impl_config->dbus_name,
                                                     context->lockdown_impl));
#endif

      export_portal_implementation (context,
                                    camera_create (connection,
                                                   access_impl_config->dbus_name,
                                                   context->lockdown_impl));

      impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Screenshot");
      if (impl_config != NULL)
        export_portal_implementation (context,
                                      screenshot_create (connection,
                                                         access_impl_config->dbus_name,
                                                         impl_config->dbus_name));

      impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Background");
      if (impl_config != NULL)
        export_portal_implementation (context,
                                      background_create (connection,
                                                         access_impl_config->dbus_name,
                                                         impl_config->dbus_name));

      impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Wallpaper");
      if (impl_config != NULL)
        export_portal_implementation (context,
                                      wallpaper_create (connection,
                                                        access_impl_config->dbus_name,
                                                        impl_config->dbus_name));
    }

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Account");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  account_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Email");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  email_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Secret");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  secret_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.GlobalShortcuts");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  global_shortcuts_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.DynamicLauncher");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  dynamic_launcher_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.ScreenCast");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  screen_cast_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.RemoteDesktop");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  remote_desktop_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Clipboard");
  if (impl_config != NULL)
    export_portal_implementation (
        context, clipboard_create (connection, impl_config->dbus_name));

  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.InputCapture");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  input_capture_create (connection, impl_config->dbus_name));

#if HAVE_GUDEV
  impl_config = xdp_portal_config_find (portal_config, "org.freedesktop.impl.portal.Usb");
  if (impl_config != NULL)
    export_portal_implementation (context,
                                  xdp_usb_create (connection, impl_config->dbus_name));
#endif

  export_host_portal_implementation (context, registry_create (context));

  return TRUE;
}
