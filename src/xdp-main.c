/*
 * Copyright © 2016 Red Hat, Inc
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
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

#include <glib-unix.h>
#include <glib/gi18n.h>
#include <libdex.h>

#include "xdp-context.h"

typedef struct _XdpMain
{
  int exit_status;
  GMainLoop *loop;
  XdpContext *context;
} XdpMain;

static void
message_handler (const char     *log_domain,
                 GLogLevelFlags  log_level,
                 const char     *message,
                 gpointer        user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    fprintf (stderr, "XDP: %s\n", message);
  else
    fprintf (stderr, "%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const char *string)
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

static void
xdp_main_exit (XdpMain *xdp_main,
               int      status)
{
  xdp_main->exit_status = status;
  g_main_loop_quit (xdp_main->loop);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  XdpMain *xdp_main = user_data;
  g_autoptr(GError) error = NULL;

  if (!xdp_context_register (xdp_main->context, connection, &error))
    {
      g_critical ("Starting portals failed: %s", error->message);
      xdp_main_exit (xdp_main, EXIT_FAILURE);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  g_debug ("%s acquired", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  XdpMain *xdp_main = user_data;

  xdp_main_exit (xdp_main, EXIT_SUCCESS);
  g_debug ("Terminated because dbus name was lost.");
}

static gboolean
on_signal (gpointer user_data)
{
  XdpMain *xdp_main = user_data;

  xdp_main_exit (xdp_main, EXIT_SUCCESS);
  g_debug ("Terminated with signal.");

  return G_SOURCE_REMOVE;
}

int
main (int argc, char *argv[])
{
  XdpMain xdp_main;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(XdpContext) context = NULL;
  guint owner_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GSource) signal_handler_source = NULL;
  g_autoptr(GOptionContext) option_context = NULL;

  gboolean opt_verbose = FALSE;
  gboolean opt_replace = FALSE;
  gboolean opt_show_version = FALSE;

  GOptionEntry entries[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace a running instance", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &opt_show_version, "Show program version.", NULL},
    { NULL }
  };

  if (g_getenv ("XDG_DESKTOP_PORTAL_WAIT_FOR_DEBUGGER") != NULL)
    {
      g_printerr ("\ndesktop portal (PID %d) is waiting for a debugger. "
                  "Use `gdb -p %d` to connect. \n",
                  getpid (), getpid ());

      if (raise (SIGSTOP) == -1)
        {
          g_printerr ("Failed waiting for debugger\n");
          exit (1);
        }

      raise (SIGCONT);
    }

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  dex_init ();

  /* Note: if you add any more environment variables here, update
   * handle_launch() in dynamic-launcher.c to unset them before launching apps
   */
  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  /* Avoid pointless and confusing recursion */
  g_unsetenv ("GTK_USE_PORTAL");

  option_context = g_option_context_new ("- desktop portal");
  g_option_context_set_summary (option_context,
    "A portal service for flatpak and other desktop containment frameworks.");
  g_option_context_set_description (option_context,
    "xdg-desktop-portal works by exposing D-Bus interfaces known as portals\n"
    "under the well-known name " DESKTOP_DBUS_NAME " and object\n"
    "path " DESKTOP_DBUS_PATH ".\n"
    "\n"
    "Documentation for the available D-Bus interfaces can be found at\n"
    "https://flatpak.github.io/xdg-desktop-portal/docs/\n"
    "\n"
    "Please report issues at https://github.com/flatpak/xdg-desktop-portal/issues");
  g_option_context_add_main_entries (option_context, entries, NULL);
  if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      return 1;
    }

  if (opt_show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  g_set_printerr_handler (printerr_handler);

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  loop = g_main_loop_new (NULL, FALSE);
  context = xdp_context_new (opt_verbose);

  xdp_main.exit_status = 0;
  xdp_main.loop = loop;
  xdp_main.context = context;

  /* Setup a signal handler so that we can quit cleanly.
   * This is useful for triggering asan.
   */
  signal_handler_source = g_unix_signal_source_new (SIGHUP);
  g_source_set_callback (signal_handler_source,
                         G_SOURCE_FUNC (on_signal),
                         &xdp_main,
                         NULL);
  g_source_attach (signal_handler_source, g_main_loop_get_context (loop));

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      return 2;
    }

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             DESKTOP_DBUS_NAME,
                             (G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                              (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0)),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             &xdp_main,
                             NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  return xdp_main.exit_status;
}
