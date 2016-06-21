#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "flatpak-utils.h"
#include "xdp-dbus.h"
#include "file-chooser.h"
#include "request.h"

static GMainLoop *loop = NULL;
XdpDocuments *documents = NULL;
char *documents_mountpoint = NULL;

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
  char *dbus_name;
  char **interfaces;
  char **use_in;
} PortalImplementation;

static void
portal_implementation_free (PortalImplementation *impl)
{
  g_free (impl->dbus_name);
  g_free (impl->interfaces);
  g_free (impl->use_in);
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
    }

  impl->use_in = g_key_file_get_string_list (keyfile, "portal", "UseIn", NULL, error);
  if (impl->use_in == NULL)
    return FALSE;

  if (opt_verbose)
    {
      g_autofree char *uses = g_strjoinv (", ", impl->use_in);
      g_debug ("portal for %s", uses);
      for (i = 0; impl->interfaces[i]; i++)
        g_debug ("portal supports %s", impl->interfaces[i]);
    }

  implementations = g_list_prepend (implementations, impl);
  impl = NULL;

  return TRUE;
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
          g_warning ("error loading %s: %s", path, error->message);
          continue;
        }
    }
}

static PortalImplementation *
find_portal (const char *interface)
{
  const char *desktops_str = g_getenv ("XDG_SESSION_DESKTOP");
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

          if (!g_strv_contains ((const char **)impl->use_in, desktops[i]))
            return impl;
        }
    }

  /* Fall back to *any* installed implementation */
  for (l = implementations; l != NULL; l = l->next)
    {
      PortalImplementation *impl = l->data;

      if (!g_strv_contains ((const char **)impl->interfaces, interface))
        continue;

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

  app_id = flatpak_invocation_lookup_app_id_sync (invocation, NULL, &error);
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
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  PortalImplementation *implementation;
  g_autoptr(GError) error = NULL;

  implementation = find_portal ("org.freedesktop.impl.portal.FileChooser");
  if (implementation != NULL)
    {
      GDBusInterfaceSkeleton *skeleton = file_chooser_create (connection, implementation->dbus_name);
      if (skeleton != NULL)
        {
          g_dbus_interface_skeleton_set_flags (skeleton,
                                               G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
          g_signal_connect (skeleton, "g-authorize-method",
                            G_CALLBACK (authorize_callback),
                            NULL);

          if (!g_dbus_interface_skeleton_export (skeleton,
                                                 connection,
                                                 "/org/freedesktop/portal/desktop",
                                                 &error))
            {
              g_warning ("error: %s\n", error->message);
              g_clear_error (&error);
            }
        }
    }

  flatpak_connection_track_name_owners (connection);

  documents = xdp_documents_proxy_new_sync (connection, 0,
                                            "org.freedesktop.portal.Documents",
                                            "/org/freedesktop/portal/documents",
                                            NULL, NULL);
  xdp_documents_call_get_mount_point_sync (documents, &documents_mountpoint, NULL, NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("org.freedesktop.portal.desktop acquired");


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
  GDBusConnection  *session_bus;
  GOptionContext *context;

  setlocale (LC_ALL, "");

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

  return 0;
}
