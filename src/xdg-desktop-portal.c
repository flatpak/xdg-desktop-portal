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


static GMainLoop *loop = NULL;
static XdpDbusDocuments *documents = NULL;
static char *mountpoint = NULL;

static gboolean opt_verbose;
static gboolean opt_replace;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
  { NULL }
};

static struct {
  const char *name;
  GDBusNodeInfo *info;
} portal_interfaces [] = {
  { "org.freedesktop.portal.FileChooser" },
  { "org.freedesktop.portal.AppChooser" },
  { "org.freedesktop.portal.Print" },
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

static void
backend_call_callback (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);
  GDBusMethodInvocation *invocation = user_data;
  GVariant *retval;
  GError *error = NULL;

  retval = g_dbus_connection_call_finish (connection, res, &error);

  if (retval == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
    }
  else
    g_dbus_method_invocation_return_value (invocation, retval);
}

typedef struct {
  char *dbus_name;
  char **interfaces;
  char **use_in;
  gboolean subscribed;
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

typedef struct {
  GDBusConnection *connection;
  char *sender_name;
  char *object_path;
  char *signal_name;
  char *app_id;
  GVariant *parameters;
} FileResponseData;

static void
file_response_data_free (FileResponseData *data)
{
  g_object_unref (data->connection);
  g_free (data->sender_name);
  g_free (data->object_path);
  g_free (data->signal_name);
  g_free (data->app_id);
  g_variant_unref (data->parameters);

  g_free (data);
}

static char *
register_document (const char *uri,
                   gboolean for_save,
                   const char *app_id,
                   gboolean writable,
                   GError **error)
{
  g_autofree char *doc_id = NULL;
  g_autofree char *path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *dirname = NULL;
  GUnixFDList *fd_list = NULL;
  int fd, fd_in;
  g_autoptr(GFile) file = NULL;
  gboolean ret = FALSE;
  const char *permissions[5];
  g_autofree char *fuse_path = NULL;
  int i;

  if (app_id == NULL)
    return g_strdup (uri);

  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  basename = g_path_get_basename (path);
  dirname = g_path_get_dirname (path);

  if (for_save)
    fd = open (dirname, O_PATH | O_CLOEXEC);
  else
    fd = open (path, O_PATH | O_CLOEXEC);

  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to open %s", uri);
      return NULL;
    }

  fd_list = g_unix_fd_list_new ();
  fd_in = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (fd_in == -1)
    return NULL;

  i = 0;
  permissions[i++] = "read";
  if (writable)
    permissions[i++] = "write";
  permissions[i++] = "grant";
  permissions[i++] = NULL;

  if (for_save)
    ret = xdp_dbus_documents_call_add_named_sync (documents,
                                                  g_variant_new_handle (fd_in),
                                                  basename,
                                                  TRUE,
                                                  TRUE,
                                                  fd_list,
                                                  &doc_id,
                                                  NULL,
                                                  NULL,
                                                  error);
  else
    ret = xdp_dbus_documents_call_add_sync (documents,
                                            g_variant_new_handle (fd_in),
                                            TRUE,
                                            TRUE,
                                            fd_list,
                                            &doc_id,
                                            NULL,
                                            NULL,
                                            error);
  g_object_unref (fd_list);

  if (!ret)
    return NULL;

  if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                       doc_id,
                                                       app_id,
                                                       permissions,
                                                       NULL,
                                                       error))
    return NULL;

  return g_build_filename (mountpoint, doc_id, basename, NULL);
}

static void
send_filechooser_response (GTask *task,
                           gpointer source,
                           gpointer task_data,
                           GCancellable *cancellable)
{
  FileResponseData *data = task_data;
  GVariantBuilder b;
  const char *handle;
  guint32 response;
  GVariant *options;
  gboolean writable;
  gboolean for_save;
  GError *error = NULL;
  const char *destination;

  g_print ("%s", g_variant_print (data->parameters, FALSE));
  g_variant_get_child (data->parameters, 0, "&s", &destination);
  g_variant_get_child (data->parameters, 1, "&o", &handle);
  g_variant_get_child (data->parameters, 2, "u", &response);
  options = g_variant_get_child_value (data->parameters, 4);

  if (strcmp (data->signal_name, "SaveFileResponse") == 0)
    for_save = TRUE;
  else
    for_save = FALSE;

  if (strcmp (data->signal_name, "SaveFileResponse") == 0)
    writable = TRUE;
  else if (!g_variant_lookup (options, "b", "writable", &writable))
    writable = FALSE;

  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&b, "s", handle);
  g_variant_builder_add (&b, "u", response);

  if (strcmp (data->signal_name, "OpenFilesResponse") == 0)
    {
      GVariantBuilder a;
      const char **uris;
      int i;

      g_variant_builder_init (&a, G_VARIANT_TYPE ("as"));
      g_variant_get_child (data->parameters, 3, "^a&s", &uris);
      for (i = 0; uris[i]; i++)
        {
          g_autofree char *ruri = NULL;

          ruri = register_document (uris[i], for_save, data->app_id, writable, &error);
          if (ruri == NULL)
            {
              g_warning ("Failed to register %s: %s\n", uris[i], error->message);
              g_clear_error (&error);
            }
          else
            {
              g_print ("convert %s -> %s\n", uris[i], ruri);
              g_variant_builder_add (&a, "s", ruri);
            }
        }

      g_variant_builder_add (&b, "@as", g_variant_builder_end (&a));
    }
  else
    {
      const char *uri;
      g_autofree char *ruri = NULL;

      g_variant_get_child (data->parameters, 3, "&s", &uri);
      ruri = register_document (uri, for_save, data->app_id, writable, &error);
      if (ruri == NULL)
        {
          g_warning ("Failed to register %s: %s\n", uri, error->message);
          g_clear_error (&error);
        }
      else
        {
          g_print ("convert %s -> %s\n", uri, ruri);
          g_variant_builder_add (&b, "s", ruri);
        }
    }

  g_variant_builder_add (&b, "@a{sv}", options);

  if (!g_dbus_connection_emit_signal (data->connection,
                                      destination,
                                      "/org/freedesktop/portal/desktop",
                                      "org.freedesktop.portal.FileChooser",
                                      data->signal_name,
                                      g_variant_builder_end (&b),
                                      &error))
    {
      g_warning ("Error emitting signal: %s\n", error->message);
      g_error_free (error);
    }
}

static void
got_app_id_for_destination (GObject *source_object,
                            GAsyncResult *res,
                            gpointer user_data)
{
  FileResponseData *data = user_data;
  g_autoptr(GTask) task = NULL;

  data->app_id = flatpak_connection_lookup_app_id_finish (data->connection, res, NULL);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify)file_response_data_free);
  g_task_run_in_thread (task, send_filechooser_response);
}

static void
handle_backend_signal (GDBusConnection  *connection,
                       const gchar      *sender_name,
                       const gchar      *object_path,
                       const gchar      *interface_name,
                       const gchar      *signal_name,
                       GVariant         *parameters,
                       gpointer          user_data)
{
  GVariantBuilder b;
  const char *destination;
  gsize n_children, i;
  GError *error = NULL;
  char *real_interface_name;

  if (strcmp (interface_name, "org.freedesktop.impl.portal.FileChooser") == 0)
    {
      FileResponseData *data;

      data = g_new0 (FileResponseData, 1);
      data->connection = g_object_ref (connection);
      data->sender_name = g_strdup (sender_name);
      data->object_path = g_strdup (object_path);
      data->signal_name = g_strdup (signal_name);
      data->parameters = g_variant_ref (parameters);

      g_variant_get_child (parameters, 0, "&s", &destination);
      flatpak_connection_lookup_app_id (connection,
                                        destination,
                                        NULL,
                                        got_app_id_for_destination,
                                        data);
      return;
    }

  g_variant_get_child (parameters, 0, "&s", &destination);

  /* Strip out destination */
  n_children = g_variant_n_children (parameters);
  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);
  for (i = 1; i < n_children; i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (parameters, i));

  if (g_str_has_prefix (interface_name, "org.freedesktop.impl.portal."))
    real_interface_name = g_strconcat ("org.freedesktop.portal.", interface_name + strlen ("org.freedesktop.impl.portal."), NULL);
  else
    real_interface_name = g_strdup (interface_name);

  if (!g_dbus_connection_emit_signal (connection, destination,
                                      "/org/freedesktop/portal/desktop",
                                      real_interface_name,
                                      signal_name,
                                      g_variant_builder_end (&b),
                                      &error))
    {
      g_warning ("Error emitting signal: %s\n", error->message);
      g_error_free (error);
    }

  g_free (real_interface_name);
}

static void
got_app_id (GObject *source_object,
            GAsyncResult *res,
            gpointer user_data)
{
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (source_object);
  char *app_id;
  char *interface_name;
  GError *error = NULL;
  GVariant *params;
  GVariantBuilder b;
  gsize n_children, i;
  const char *real_iface;
  PortalImplementation *implementation;

  app_id = flatpak_invocation_lookup_app_id_finish (invocation, res, &error);
  if (app_id == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      return;
    }

  real_iface = g_dbus_method_invocation_get_interface_name (invocation);

  if (!g_str_has_prefix (real_iface, "org.freedesktop.portal."))
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR,
                                             G_IO_ERROR_NOT_SUPPORTED,
                                             "Interface %s is not supported by any implementation", real_iface);
      return;
    }

  implementation = find_portal (real_iface);
  if (implementation == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR,
                                             G_IO_ERROR_NOT_SUPPORTED,
                                             "Interface %s is not supported by any implementation", real_iface);
      return;
    }

  if (!implementation->subscribed)
    {
      g_dbus_connection_signal_subscribe (g_dbus_method_invocation_get_connection (invocation),
                                          implementation->dbus_name,
                                          NULL,
                                          NULL,
                                          "/org/freedesktop/portal/desktop",
                                          NULL,
                                          G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                          handle_backend_signal,
                                          implementation, NULL);
      implementation->subscribed = TRUE;
    }

  interface_name = g_strconcat ("org.freedesktop.impl.portal.", real_iface + strlen ("org.freedesktop.portal."), NULL);

  params = g_dbus_method_invocation_get_parameters (invocation);

  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);

  g_variant_builder_add_value (&b, g_variant_new_string (g_dbus_method_invocation_get_sender (invocation)));
  g_variant_builder_add_value (&b, g_variant_new_string (app_id));

  n_children = g_variant_n_children (params);

  for (i = 0; i < n_children; i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (params, i));

  g_dbus_connection_call (g_dbus_method_invocation_get_connection (invocation),
                          implementation->dbus_name,
                          g_dbus_method_invocation_get_object_path (invocation),
                          interface_name,
                          g_dbus_method_invocation_get_method_name (invocation),
                          g_variant_builder_end (&b),
                          NULL, // TODO: reply_type from method_info
                          G_DBUS_CALL_FLAGS_NONE,
                          G_MAXINT,
                          NULL,
                          backend_call_callback,
                          invocation);
  g_free (interface_name);
}

static void
method_call (GDBusConnection       *connection,
             const gchar           *sender,
             const gchar           *object_path,
             const gchar           *interface_name,
             const gchar           *method_name,
             GVariant              *parameters,
             GDBusMethodInvocation *invocation,
             gpointer               user_data)
{
  g_print ("method call %s %s\n", interface_name, method_name);
  flatpak_invocation_lookup_app_id (invocation, NULL,
                                    got_app_id, invocation);

}

static GDBusInterfaceVTable wrapper_vtable = {
  method_call,
  NULL,
  NULL,
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;
  guint id;
  int i;

  for (i = 0; portal_interfaces[i].name != NULL; i++)
    {
      GDBusInterfaceInfo *iface_info;
      iface_info =
        g_dbus_node_info_lookup_interface (portal_interfaces[i].info, portal_interfaces[i].name);
      g_assert (iface_info != NULL);

      id = g_dbus_connection_register_object (connection,
                                              "/org/freedesktop/portal/desktop",
                                              iface_info,
                                              &wrapper_vtable,
                                              NULL, NULL, &error);
      if (id == 0)
        {
          g_warning ("error registering object: %s\n", error->message);
          g_clear_error (&error);
        }
    }

  flatpak_connection_track_name_owners (connection);

  documents = xdp_dbus_documents_proxy_new_sync (connection, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, NULL);
  xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint, NULL, NULL);
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
  int i;

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

  for (i = 0; portal_interfaces[i].name != NULL; i++)
    {
      g_autofree char *path = g_strdup_printf ("/org/freedesktop/portal/desktop/%s.xml", portal_interfaces[i].name);
      g_autoptr(GBytes) introspection_bytes = NULL;
      GDBusNodeInfo *info;

      introspection_bytes = g_resources_lookup_data (path, 0, NULL);
      g_assert (introspection_bytes != NULL);

      info = g_dbus_node_info_new_for_xml (g_bytes_get_data (introspection_bytes, NULL), NULL);
      g_assert (info != NULL);
      portal_interfaces[i].info = info;
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
