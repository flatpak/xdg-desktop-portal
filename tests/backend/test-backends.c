#include <config.h>
#include <stdio.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "access.h"
#include "account.h"
#include "appchooser.h"
#include "background.h"
#include "email.h"
#include "filechooser.h"
#include "inhibit.h"
#include "lockdown.h"
#include "notification.h"
#include "print.h"
#include "screenshot.h"
#include "settings.h"
#include "wallpaper.h"

#include "src/glib-backports.h"

#define BACKEND_OBJECT_PATH "/org/freedesktop/portal/desktop"

static GMainLoop *loop;

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  access_init (connection, BACKEND_OBJECT_PATH);
  account_init (connection, BACKEND_OBJECT_PATH);
  appchooser_init (connection, BACKEND_OBJECT_PATH);
  background_init (connection, BACKEND_OBJECT_PATH);
  email_init (connection, BACKEND_OBJECT_PATH);
  file_chooser_init (connection, BACKEND_OBJECT_PATH);
  inhibit_init (connection, BACKEND_OBJECT_PATH);
  lockdown_init (connection, BACKEND_OBJECT_PATH);
  notification_init (connection, BACKEND_OBJECT_PATH);
  print_init (connection, BACKEND_OBJECT_PATH);
  screenshot_init (connection, BACKEND_OBJECT_PATH);
  settings_init (connection, BACKEND_OBJECT_PATH);
  wallpaper_init (connection, BACKEND_OBJECT_PATH);
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
  g_debug ("%s lost", name);
  g_main_loop_quit (loop);
}

static gboolean opt_verbose;
static gboolean opt_replace;
static char *opt_backend_name;

static GOptionEntry entries[] = {
  { "backend-name", 0, 0, G_OPTION_ARG_STRING, &opt_backend_name, "The name of the backend on the bus", NULL },
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace a running instance", NULL },
  { NULL }
};

static void
message_handler (const char *log_domain,
                 GLogLevelFlags log_level,
                 const char *message,
                 gpointer user_data)
{
  if (log_level & G_LOG_LEVEL_DEBUG)
    printf ("TST: %s\n", message);
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

int
main (int argc, char *argv[])
{
  guint owner_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GOptionContext) context = NULL;

  g_log_writer_default_set_use_stderr (TRUE);

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  context = g_option_context_new ("- portal test backends");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      return 1;
    }

  if (opt_backend_name == NULL)
    g_error ("You must specify the name of the backend to own on the bus");

  g_set_printerr_handler (printerr_handler);
  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  loop = g_main_loop_new (NULL, FALSE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      return 2;
    }

  g_debug ("Testing backends for '%s'", opt_backend_name);
  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             opt_backend_name,
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);
  g_main_loop_unref (loop);

  g_debug ("%s exiting.", g_get_prgname ());

  return 0;
}
