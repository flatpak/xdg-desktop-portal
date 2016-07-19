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
#include "request.h"
#include "documents.h"
#include "permissions.h"
#include "file-chooser.h"
#include "open-uri.h"
#include "print.h"
#include "network-monitor.h"
#include "proxy-resolver.h"
#include "screenshot.h"
#include "notification.h"
#include "inhibit.h"
#include "device.h"

static GMainLoop *loop = NULL;

static gboolean opt_verbose;
static gboolean opt_replace;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
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
    printf ("XDP: %s\n", message);
  else
    printf ("%s: %s\n", g_get_prgname (), message);
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

typedef struct {
  char *source;
  char *dbus_name;
  char **interfaces;
  char **use_in;
  int priority;
} PortalImplementation;

static void
portal_implementation_free (PortalImplementation *impl)
{
  g_free (impl->source);
  g_free (impl->dbus_name);
  g_strfreev (impl->interfaces);
  g_strfreev (impl->use_in);
  g_free (impl);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalImplementation, portal_implementation_free)

static GList *implementations = NULL;

static gboolean
register_portal (const char *path, GError **error)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(PortalImplementation) impl = g_new0 (PortalImplementation, 1);
  int i;

  g_debug ("loading %s", path);

  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;

  impl->source = g_path_get_basename (path);
  impl->dbus_name = g_key_file_get_string (keyfile, "portal", "DBusName", error);
  if (impl->dbus_name == NULL)
    return FALSE;
  if (!g_dbus_is_name (impl->dbus_name))
    {
      g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                   "Not a valid bus name: %s", impl->dbus_name);
      return FALSE;
    }

  impl->interfaces = g_key_file_get_string_list (keyfile, "portal", "Interfaces", NULL, error);
  if (impl->interfaces == NULL)
    return FALSE;
  for (i = 0; impl->interfaces[i]; i++)
    {
      if (!g_dbus_is_interface_name (impl->interfaces[i]))
        {
          g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                       "Not a valid interface name: %s", impl->interfaces[i]);
          return FALSE;
        }
      if (!g_str_has_prefix (impl->interfaces[i], "org.freedesktop.impl.portal."))
        {
          g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                       "Not a portal backend interface: %s", impl->interfaces[i]);
          return FALSE;
        }
    }

  impl->use_in = g_key_file_get_string_list (keyfile, "portal", "UseIn", NULL, error);
  if (impl->use_in == NULL)
    return FALSE;

  if (opt_verbose)
    {
      g_autofree char *uses = g_strjoinv (", ", impl->use_in);
      g_debug ("portal implementation for %s", uses);
      for (i = 0; impl->interfaces[i]; i++)
        g_debug ("portal implementation supports %s", impl->interfaces[i]);
    }

  implementations = g_list_prepend (implementations, impl);
  impl = NULL;

  return TRUE;
}

static gint
sort_impl_by_name (gconstpointer a,
                   gconstpointer b)
{
  const PortalImplementation *pa = a;
  const PortalImplementation *pb = b;

  return strcmp (pa->source, pb->source);
}

static void
load_installed_portals (void)
{
  const char *portal_dir = PKGDATADIR "/portals";
  g_autoptr(GFile) dir = g_file_new_for_path (portal_dir);
  g_autoptr(GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children (dir, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (enumerator == NULL)
    return;

  while (TRUE)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (enumerator, NULL, NULL);
      g_autoptr(GFile) child = NULL;
      g_autofree char *path = NULL;
      const char *name;
      g_autoptr(GError) error = NULL;

      if (info == NULL)
        break;

      name = g_file_info_get_name (info);

      if (!g_str_has_suffix (name, ".portal"))
        continue;

      child = g_file_enumerator_get_child (enumerator, info);
      path = g_file_get_path (child);

      if (!register_portal (path, &error))
        {
          g_warning ("Error loading %s: %s", path, error->message);
          continue;
        }
    }

  implementations = g_list_sort (implementations, sort_impl_by_name);
}

static gboolean
g_strv_case_contains (const gchar * const *strv,
                      const gchar         *str)
{
  for (; *strv != NULL; strv++)
    {
      if (g_ascii_strcasecmp (str, *strv) == 0)
        return TRUE;
    }

  return FALSE;
}

static PortalImplementation *
find_portal_implementation (const char *interface)
{
  const char *desktops_str = g_getenv ("XDG_CURRENT_DESKTOP");
  g_auto(GStrv) desktops = NULL;
  int i;
  GList *l;

  if (desktops_str == NULL)
    desktops_str = "";

  desktops = g_strsplit (desktops_str, ":", -1);

  for (i = 0; desktops[i] != NULL; i++)
    {
     for (l = implementations; l != NULL; l = l->next)
        {
          PortalImplementation *impl = l->data;

          if (!g_strv_contains ((const char **)impl->interfaces, interface))
            continue;

          if (g_strv_case_contains ((const char **)impl->use_in, desktops[i]))
            {
              g_debug ("Using %s for %s in %s", impl->source, interface, desktops[i]);
              return impl;
            }
        }
    }

  /* Fall back to *any* installed implementation */
  for (l = implementations; l != NULL; l = l->next)
    {
      PortalImplementation *impl = l->data;

      if (!g_strv_contains ((const char **)impl->interfaces, interface))
        continue;

      g_debug ("Falling back to %s for %s", impl->source, interface);
      return impl;
    }

  return NULL;
}

static gboolean
authorize_callback (GDBusInterfaceSkeleton *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  g_autofree char *app_id;

  g_autoptr(GError) error = NULL;

  app_id = xdp_invocation_lookup_app_id_sync (invocation, NULL, &error);
  if (app_id == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: %s", error->message);
      return FALSE;
    }

  request_init_invocation (invocation, app_id);

  return TRUE;
}

static void
export_portal_implementation (GDBusConnection *connection,
                              GDBusInterfaceSkeleton *skeleton)
{
  g_autoptr(GError) error = NULL;

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
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  PortalImplementation *implementation;
  g_autoptr(GError) error = NULL;

  xdp_connection_track_name_owners (connection);
  init_document_proxy (connection);
  init_permission_store (connection);

  export_portal_implementation (connection, network_monitor_create (connection));
  export_portal_implementation (connection, proxy_resolver_create (connection));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.FileChooser");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  file_chooser_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.AppChooser");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  open_uri_create (connection, implementation->dbus_name));

  implementation = find_portal_implementation ("org.freedesktop.impl.portal.Print");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  print_create (connection, implementation->dbus_name));

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
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  device_create (connection, implementation->dbus_name));
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

  g_set_printerr_handler (printerr_handler);

  context = g_option_context_new ("- desktop portal");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s", error->message);
      return 1;
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  load_installed_portals ();

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
