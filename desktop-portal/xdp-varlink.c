/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-varlink.h"

#include <string.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <libdex.h>
#include <varlink.h>

#include "xdp-dex.h"

/* Sentinel varargs cannot be built from an array, so add_interface switches
 * on the method count */
#define MAX_INTERFACE_METHODS 12

struct _XdpVarlinkService
{
  GObject parent_instance;

  char *socket_path;
  VarlinkService *service;
  GMainContext *context;
  GSource *listen_source;
  XdpAppInfoRegistry *app_info_registry;
  GPtrArray *interfaces;

  /* Owned, one entry per accepted connection */
  GHashTable *connections;
};

/* A fiber exists only while a batch is dispatched, so an idle connection
 * costs a source rather than a stack */
typedef struct
{
  XdpVarlinkService *service;

  VarlinkServiceConnection *varlink_connection;
  XdpVarlinkConnection *connection;

  /* Owned, and armed only while no fiber is dispatching this connection */
  GSource *source;
  uint32_t armed_events;

  GIOCondition revents;
} ConnectionEntry;

static void
connection_entry_free (gpointer data)
{
  ConnectionEntry *entry = data;

  if (entry->source)
    {
      g_source_destroy (entry->source);
      g_clear_pointer (&entry->source, g_source_unref);
    }

  varlink_service_connection_set_closed_callback (entry->varlink_connection,
                                                  NULL, NULL);
  varlink_service_connection_unref (entry->varlink_connection);

  g_clear_object (&entry->connection);
  g_free (entry);
}

/* Every method of an interface dispatches through this, told apart by name */
typedef struct
{
  XdpVarlinkService *service;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
  XdpVarlinkMethod *methods;
  size_t n_methods;
} InterfaceEntry;

G_DEFINE_FINAL_TYPE (XdpVarlinkService, xdp_varlink_service, G_TYPE_OBJECT);

static void
interface_entry_free (gpointer data)
{
  InterfaceEntry *entry = data;

  if (entry->user_data_destroy)
    entry->user_data_destroy (entry->user_data);

  g_free (entry->methods);
  g_free (entry);
}

/* The called method's name is fully qualified, so it carries the interface
 * the reply error has to belong to */
static char *
not_registered_error (VarlinkCall *call)
{
  const char *qualified = varlink_call_get_method (call);
  const char *dot = strrchr (qualified, '.');

  if (dot == NULL)
    return NULL;

  return g_strdup_printf ("%.*s." XDP_VARLINK_ERROR_NOT_REGISTERED,
                          (int) (dot - qualified),
                          qualified);
}

static long
reject_unregistered (VarlinkCall *call)
{
  g_autofree char *error = not_registered_error (call);

  if (error == NULL)
    return -VARLINK_ERROR_INVALID_CALL;

  return varlink_call_reply_error (call, error, NULL);
}

static const XdpVarlinkMethod *
find_method (const InterfaceEntry *interface,
             VarlinkCall          *call)
{
  const char *qualified = varlink_call_get_method (call);
  const char *name = strrchr (qualified, '.');

  if (name == NULL)
    return NULL;
  name++;

  for (size_t i = 0; i < interface->n_methods; i++)
    {
      if (strcmp (interface->methods[i].name, name) == 0)
        return &interface->methods[i];
    }

  return NULL;
}

static long
dispatch_method (VarlinkService *varlink_service,
                 VarlinkCall    *call,
                 VarlinkObject  *parameters,
                 uint64_t        flags,
                 void           *userdata)
{
  InterfaceEntry *interface = userdata;
  XdpVarlinkService *self = interface->service;
  const XdpVarlinkMethod *method;
  XdpVarlinkConnection *connection;
  ConnectionEntry *entry;

  /* Only reached for a method registered from interface->methods */
  method = find_method (interface, call);
  g_assert (method != NULL);

  entry = varlink_service_connection_get_userdata (varlink_call_get_connection (call));
  g_assert (entry != NULL);
  connection = entry->connection;

  if (xdp_varlink_connection_get_instance (connection) == NULL &&
      !(method->flags & XDP_VARLINK_METHOD_FLAGS_PRE_CLAIM))
    return reject_unregistered (call);

  return method->func (self, connection, call, parameters, flags,
                       interface->user_data);
}

static void connection_entry_arm (ConnectionEntry *entry);

/* Handlers await inside process_events(), which keeps libvarlink draining
 * pipelined messages while only this connection waits */
static DexFuture *
dispatch_fiber (XdpVarlinkService *self,
                gpointer           data)
{
  ConnectionEntry *entry = data;
  long res;

  res = varlink_service_connection_process_events (entry->varlink_connection,
                                                   entry->revents);
  if (res < 0 && res != -VARLINK_ERROR_CONNECTION_CLOSED)
    {
      g_debug ("Failed to process varlink events on %s: %s",
               self->socket_path,
               varlink_error_string (-res));
      varlink_service_connection_close (entry->varlink_connection);
    }

  if (varlink_service_connection_is_closed (entry->varlink_connection))
    g_hash_table_remove (self->connections, entry);
  else
    connection_entry_arm (entry);

  return dex_future_new_true ();
}

static gboolean
on_connection_ready (int          fd,
                     GIOCondition condition,
                     gpointer     user_data)
{
  ConnectionEntry *entry = user_data;

  /* Nothing else may dispatch this connection until the fiber is done */
  g_clear_pointer (&entry->source, g_source_unref);
  entry->revents = condition;

  dex_future_disown (dex_scheduler_spawnv (NULL, 0,
                                           G_CALLBACK (dispatch_fiber),
                                           2,
                                           XDP_TYPE_VARLINK_SERVICE, entry->service,
                                           G_TYPE_POINTER, entry));

  return G_SOURCE_REMOVE;
}

static void
connection_entry_arm (ConnectionEntry *entry)
{
  uint32_t events;

  g_assert (entry->source == NULL);

  events = varlink_service_connection_get_events (entry->varlink_connection);
  entry->armed_events = events;

  entry->source =
    g_unix_fd_source_new (varlink_service_connection_get_fd (entry->varlink_connection),
                          events);
  g_source_set_callback (entry->source,
                         G_SOURCE_FUNC (on_connection_ready),
                         entry,
                         NULL);
  g_source_attach (entry->source, entry->service->context);
}

long
xdp_varlink_call_reply (VarlinkCall   *call,
                        VarlinkObject *parameters,
                        uint64_t       flags)
{
  VarlinkServiceConnection *varlink_connection = varlink_call_get_connection (call);
  ConnectionEntry *entry;
  long res;

  if (varlink_service_connection_is_closed (varlink_connection))
    return -VARLINK_ERROR_CONNECTION_CLOSED;

  res = varlink_call_reply (call, parameters, flags);

  entry = varlink_service_connection_get_userdata (varlink_connection);
  if (entry->source != NULL &&
      entry->armed_events != varlink_service_connection_get_events (varlink_connection))
    {
      g_source_destroy (entry->source);
      g_clear_pointer (&entry->source, g_source_unref);
      connection_entry_arm (entry);
    }

  return res;
}

VarlinkObject *
xdp_varlink_object_new_for_variant (GVariant *value)
{
  g_autoptr(JsonNode) root = json_node_new (JSON_NODE_OBJECT);
  g_autoptr(JsonObject) object = json_object_new ();
  g_autofree char *json = NULL;
  VarlinkObject *result = NULL;

  /* TODO: Figure out a better way to handle uint64 */
  json_object_set_member (object, "v", json_gvariant_serialize (value));
  json_node_take_object (root, g_steal_pointer (&object));
  json = json_to_string (root, FALSE);

  if (varlink_object_new_from_json (&result, json) < 0)
    return NULL;

  return result;
}

static gboolean
on_listen_ready (int fd,
                 GIOCondition condition,
                 gpointer user_data)
{
  XdpVarlinkService *self = XDP_VARLINK_SERVICE (user_data);

  for (;;)
    {
      VarlinkServiceConnection *varlink_connection = NULL;
      ConnectionEntry *entry;
      guint64 cookie;
      long res;
      int connection_fd;

      res = varlink_service_accept (self->service, &varlink_connection);
      if (res < 0)
        {
          g_debug ("Failed to accept a varlink connection on %s: %s",
                   self->socket_path,
                   varlink_error_string (-res));
          break;
        }

      if (res == 0)
        break;

      connection_fd = varlink_service_connection_get_fd (varlink_connection);
      if (!xdp_varlink_socket_cookie (connection_fd, &cookie))
        {
          varlink_service_connection_close (varlink_connection);
          varlink_service_connection_unref (varlink_connection);
          continue;
        }

      entry = g_new0 (ConnectionEntry, 1);
      entry->service = self;
      entry->varlink_connection = varlink_connection;
      entry->connection = xdp_varlink_connection_new (self->app_info_registry,
                                                      connection_fd,
                                                      cookie);

      /* Carries the entry to the dispatcher, which only sees the call */
      varlink_service_connection_set_closed_callback (varlink_connection,
                                                      NULL,
                                                      entry);

      g_hash_table_add (self->connections, entry);
      connection_entry_arm (entry);
    }

  return G_SOURCE_CONTINUE;
}

static void
xdp_varlink_service_dispose (GObject *object)
{
  XdpVarlinkService *self = XDP_VARLINK_SERVICE (object);

  if (self->listen_source)
    {
      g_source_destroy (self->listen_source);
      g_clear_pointer (&self->listen_source, g_source_unref);
    }

  /* Closes every connection, so nothing dispatches while the entries go */
  g_clear_pointer (&self->service, varlink_service_free);

  /* Connections hold the last references to the instances, whose table the
   * interfaces own */
  g_clear_pointer (&self->connections, g_hash_table_unref);
  g_clear_pointer (&self->interfaces, g_ptr_array_unref);
  g_clear_object (&self->app_info_registry);
  g_clear_pointer (&self->context, g_main_context_unref);
  g_clear_pointer (&self->socket_path, g_free);

  G_OBJECT_CLASS (xdp_varlink_service_parent_class)->dispose (object);
}

static void
xdp_varlink_service_class_init (XdpVarlinkServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_varlink_service_dispose;
}

static void
xdp_varlink_service_init (XdpVarlinkService *self)
{
  self->interfaces = g_ptr_array_new_with_free_func (interface_entry_free);
  self->connections = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                             connection_entry_free, NULL);
}

XdpVarlinkService *
xdp_varlink_service_new (const char *socket_name,
                         XdpAppInfoRegistry *app_info_registry,
                         GError **error)
{
  g_autoptr(XdpVarlinkService) self = NULL;
  g_autofree char *address = NULL;
  long res;
  int fd;

  g_return_val_if_fail (socket_name != NULL, NULL);
  g_return_val_if_fail (XDP_IS_APP_INFO_REGISTRY (app_info_registry), NULL);

  self = g_object_new (XDP_TYPE_VARLINK_SERVICE, NULL);
  self->app_info_registry = g_object_ref (app_info_registry);
  self->socket_path =
    g_build_filename (g_get_user_runtime_dir (), socket_name, NULL);
  address = g_strconcat ("unix:", self->socket_path, NULL);

  res = varlink_service_new (&self->service,
                             "freedesktop.org",
                             "xdg-desktop-portal",
                             PACKAGE_VERSION,
                             "https://flatpak.github.io/xdg-desktop-portal/",
                             address,
                             -1);
  if (res < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to listen on %s: %s",
                   self->socket_path,
                   varlink_error_string (-res));
      return NULL;
    }

  /* Descriptors travel both ways: EIS sockets out, clipboard pipes in */
  varlink_service_set_allow_fd_passing_input (self->service, TRUE);
  varlink_service_set_allow_fd_passing_output (self->service, TRUE);

  res = varlink_service_set_external_loop (self->service, TRUE);
  if (res < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to take over the event loop for %s: %s",
                   self->socket_path,
                   varlink_error_string (-res));
      return NULL;
    }

  fd = varlink_service_get_listen_fd (self->service);
  if (fd < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "No listening descriptor for %s: %s",
                   self->socket_path,
                   varlink_error_string (-fd));
      return NULL;
    }

  self->context = g_main_context_ref_thread_default ();

  self->listen_source = g_unix_fd_source_new (fd, G_IO_IN);
  g_source_set_callback (self->listen_source,
                         G_SOURCE_FUNC (on_listen_ready),
                         self,
                         NULL);
  g_source_attach (self->listen_source, self->context);

  g_debug ("Listening for varlink calls on %s", self->socket_path);

  return g_steal_pointer (&self);
}

XdpAppInfoRegistry *
xdp_varlink_service_get_app_info_registry (XdpVarlinkService *self)
{
  g_return_val_if_fail (XDP_IS_VARLINK_SERVICE (self), NULL);

  return self->app_info_registry;
}

gboolean
xdp_varlink_service_add_interface (XdpVarlinkService      *self,
                                   const char             *description,
                                   const XdpVarlinkMethod *methods,
                                   size_t                  n_methods,
                                   gpointer                user_data,
                                   GDestroyNotify          user_data_destroy,
                                   GError                **error)
{
  g_autofree InterfaceEntry *interface = NULL;
  long res;

  g_return_val_if_fail (XDP_IS_VARLINK_SERVICE (self), FALSE);
  g_return_val_if_fail (description != NULL, FALSE);
  g_return_val_if_fail (n_methods > 0, FALSE);
  g_return_val_if_fail (n_methods <= MAX_INTERFACE_METHODS, FALSE);

  interface = g_new0 (InterfaceEntry, 1);
  interface->service = self;
  interface->user_data = user_data;
  interface->user_data_destroy = user_data_destroy;
  interface->methods = g_memdup2 (methods, n_methods * sizeof (*methods));
  interface->n_methods = n_methods;

#define M(i) methods[i].name, dispatch_method, interface
#define M1 M (0)
#define M2 M1, M (1)
#define M3 M2, M (2)
#define M4 M3, M (3)
#define M5 M4, M (4)
#define M6 M5, M (5)
#define M7 M6, M (6)
#define M8 M7, M (7)
#define M9 M8, M (8)
#define M10 M9, M (9)
#define M11 M10, M (10)
#define M12 M11, M (11)

#define ADD(n)                                                                \
    case n:                                                                   \
      res = varlink_service_add_interface (self->service, description,        \
                                           M##n, NULL);                       \
      break;

  switch (n_methods)
    {
    ADD (1)
    ADD (2)
    ADD (3)
    ADD (4)
    ADD (5)
    ADD (6)
    ADD (7)
    ADD (8)
    ADD (9)
    ADD (10)
    ADD (11)
    ADD (12)
    default:
      g_assert_not_reached ();
    }

#undef ADD
#undef M12
#undef M11
#undef M10
#undef M9
#undef M8
#undef M7
#undef M6
#undef M5
#undef M4
#undef M3
#undef M2
#undef M1
#undef M

  if (res < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to add varlink interface: %s",
                   varlink_error_string (-res));
      interface_entry_free (g_steal_pointer (&interface));
      return FALSE;
    }

  g_ptr_array_add (self->interfaces, g_steal_pointer (&interface));

  return TRUE;
}

