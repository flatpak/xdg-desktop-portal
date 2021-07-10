/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

#include <glib/gi18n.h>

#include "xdp-utils.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "request.h"
#include "call.h"
#include "portal-impl.h"
#include "documents.h"
#include "permissions.h"
#include "file-chooser.h"
#include "open-uri.h"
#include "print.h"
#include "memory-monitor.h"
#include "network-monitor.h"
#include "proxy-resolver.h"
#include "screenshot.h"
#include "notification.h"
#include "inhibit.h"
#include "device.h"
#include "account.h"
#include "email.h"
#include "screen-cast.h"
#include "remote-desktop.h"
#include "trash.h"
#include "location.h"
#include "settings.h"
#include "background.h"
#include "gamemode.h"
#include "camera.h"
#include "secret.h"
#include "wallpaper.h"
#include "realtime.h"

static GMainLoop *loop = NULL;

gboolean opt_verbose;
static gboolean opt_replace;
static gboolean show_version;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace a running instance", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
  { NULL }
};

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    fprintf (stderr, "XDP: %s\n", message);
  else
    fprintf (stderr, "%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  int is_tty = isatty (1);
  const char *prefix = "";
  const char *suffix = "";
  if (is_tty)
    {
      prefix = "\x1b[31m\x1b[1m"; /* red, bold */
      suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
    }
  fprintf (stderr, "%serror: %s%s\n", prefix, suffix, string);
}

static gboolean
method_needs_request (GDBusMethodInvocation *invocation)
{
  const char *interface;
  const char *method;

  interface = g_dbus_method_invocation_get_interface_name (invocation);
  method = g_dbus_method_invocation_get_method_name (invocation);

  if (strcmp (interface, "org.freedesktop.portal.ScreenCast") == 0)
    {
      if (strcmp (method, "OpenPipeWireRemote") == 0)
        return FALSE;
      else
        return TRUE;
    }
  else if (strcmp (interface, "org.freedesktop.portal.RemoteDesktop") == 0)
    {
      if (strstr (method, "Notify") == method)
        return FALSE;
      else
        return TRUE;
    }
  if (strcmp (interface, "org.freedesktop.portal.Camera") == 0)
    {
      if (strcmp (method, "OpenPipeWireRemote") == 0)
        return FALSE;
      else
        return TRUE;
    }
  else
    {
      return TRUE;
    }
}

static gboolean
authorize_callback (GDBusInterfaceSkeleton *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  g_autoptr(XdpAppInfo) app_info = NULL;

  g_autoptr(GError) error = NULL;

  app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, &error);
  if (app_info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: %s", error->message);
      return FALSE;
    }

  if (method_needs_request (invocation))
    request_init_invocation (invocation, app_info);
  else
    call_init_invocation (invocation, app_info);

  return TRUE;
}

static void
export_portal_implementation (GDBusConnection *connection,
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
                    G_CALLBACK (authorize_callback), NULL);

  if (!g_dbus_interface_skeleton_export (skeleton,
                                         connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Error: %s", error->message);
      return;
    }

  g_debug ("providing portal %s", g_dbus_interface_skeleton_get_info (skeleton)->name);
}

static void
peer_died_cb (const char *name)
{
  close_requests_for_sender (name);
  close_sessions_for_sender (name);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  PortalImplementation *implementation;
  PortalImplementation *implementation2;
  g_autoptr(GError) error = NULL;
  XdpImplLockdown *lockdown;
  GQuark portal_errors G_GNUC_UNUSED;
  GPtrArray *impls;

  /* make sure errors are registered */
  portal_errors = XDG_DESKTOP_PORTAL_ERROR;

  xdp_connection_track_name_owners (connection, peer_died_cb);
  init_document_proxy (connection);
  init_permission_store (connection);

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Lockdown");
  if (implementation != NULL)
    lockdown = xdp_impl_lockdown_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 implementation->dbus_name,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL, &error);
  else
    lockdown = xdp_impl_lockdown_skeleton_new ();

  export_portal_implementation (connection, memory_monitor_create (connection));
  export_portal_implementation (connection, network_monitor_create (connection));
  export_portal_implementation (connection, proxy_resolver_create (connection));
  export_portal_implementation (connection, trash_create (connection));
  export_portal_implementation (connection, game_mode_create (connection));
  export_portal_implementation (connection, realtime_create (connection));

  impls = find_all_portal_implementations ("org.freedesktop.impl.portal.Settings");
  export_portal_implementation (connection, settings_create (connection, impls));
  g_ptr_array_free (impls, TRUE);

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.FileChooser");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  file_chooser_create (connection, implementation->dbus_name, lockdown));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.AppChooser");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  open_uri_create (connection, implementation->dbus_name, lockdown));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Print");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  print_create (connection, implementation->dbus_name, lockdown));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Screenshot");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  screenshot_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Notification");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  notification_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Inhibit");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  inhibit_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Access");
  implementation2 = find_portal_implementation ("org.freedesktop.impl.portal.Background");
  if (implementation != NULL)
    {
      export_portal_implementation (connection,
                                    device_create (connection, implementation->dbus_name, lockdown));
#ifdef HAVE_GEOCLUE
      export_portal_implementation (connection,
                                    location_create (connection, implementation->dbus_name, lockdown));
#endif

#ifdef HAVE_PIPEWIRE
      export_portal_implementation (connection, camera_create (connection, lockdown));
#endif
    }

  if (implementation != NULL && implementation2 != NULL)
    export_portal_implementation (connection,
                                  background_create (connection,
                                                     implementation->dbus_name,
                                                     implementation2->dbus_name));

  implementation2 = find_portal_implementation ("org.freedesktop.impl.portal.Wallpaper");
  if (implementation != NULL && implementation2 != NULL)
    export_portal_implementation (connection,
                                  wallpaper_create (connection,
                                                    implementation->dbus_name,
                                                    implementation2->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Account");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  account_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Email");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  email_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Secret");
  if (implementation != NULL)
    export_portal_implementation (connection,
				  secret_create (connection, implementation->dbus_name));

#ifdef HAVE_PIPEWIRE
  implementation = find_portal_implementation ("org.freedesktop.impl.portal.ScreenCast");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  screen_cast_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.RemoteDesktop");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  remote_desktop_create (connection, implementation->dbus_name));
#endif
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("%s acquired", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_main_loop_quit (loop);
}

int
main (int argc, char *argv[])
{
  guint owner_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GOptionContext) context;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  /* Avoid pointless and confusing recursion */
  g_unsetenv ("GTK_USE_PORTAL");

  context = g_option_context_new ("- desktop portal");
  g_option_context_set_summary (context,
      "A portal service for flatpak and other desktop containment frameworks.");
  g_option_context_set_description (context,
      "xdg-desktop-portal works by exposing D-Bus interfaces known as portals\n"
      "under the well-known name org.freedesktop.portal.Desktop and object\n"
      "path /org/freedesktop/portal/desktop.\n"
      "\n"
      "Documentation for the available D-Bus interfaces can be found at\n"
      "https://flatpak.github.io/xdg-desktop-portal/portal-docs.html\n"
      "\n"
      "Please report issues at https://github.com/flatpak/xdg-desktop-portal/issues");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  g_set_printerr_handler (printerr_handler);

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  load_installed_portals (opt_verbose);

  loop = g_main_loop_new (NULL, FALSE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      return 2;
    }

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.portal.Desktop",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);
  g_main_loop_unref (loop);

  return 0;
}
