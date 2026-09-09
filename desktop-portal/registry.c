/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "registry.h"

#include <stdio.h>

#include "xdp-app-info-registry.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-host-dbus.h"
#include "xdp-peer-dbus-private.h"
#include "xdp-utils.h"

typedef struct _Registry Registry;
typedef struct _RegistryClass RegistryClass;

struct _Registry
{
  XdpDbusHostRegistrySkeleton parent_instance;

  XdpAppInfoRegistry *app_info_registry;
};

struct _RegistryClass
{
  XdpDbusHostRegistrySkeletonClass parent_class;
};

GType registry_get_type (void);
static void registry_iface_init (XdpDbusHostRegistryIface *iface);

G_DEFINE_TYPE_WITH_CODE (Registry, registry,
                         XDP_DBUS_HOST_TYPE_REGISTRY_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_HOST_TYPE_REGISTRY,
                                                registry_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Registry, g_object_unref)

static gboolean
handle_register (XdpDbusHostRegistry   *object,
                 GDBusMethodInvocation *invocation,
                 const char            *arg_app_id,
                 GVariant              *arg_options)
{
  Registry *registry = (Registry *) object;
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(XdpPeer) peer = NULL;
  g_autoptr(XdpAppInfo) new_app_info = NULL;
  g_autoptr(DexFuture) insert_future = NULL;
  g_autoptr(DexPromise) app_info_promise = NULL;
  gboolean success;
  g_autoptr(GError) error = NULL;

  peer = xdp_peer_dbus_new (g_dbus_method_invocation_get_connection (invocation),
                            sender);

  /* First, let's add an insert operation, that way we block any further
   * app info activity on the connection, until the promise is resolved. */
  app_info_promise = dex_promise_new ();
  insert_future =
    xdp_app_info_registry_insert_future (registry->app_info_registry,
                                         peer,
                                         DEX_FUTURE (dex_ref (app_info_promise)));

  /* Then we check if we actually should allow the caller to update the
   * app id */
  {
    g_autoptr(XdpAppInfo) detected_app_info = NULL;

    detected_app_info = dex_await_object (xdp_app_info_new_for_peer (peer), &error);
    if (!detected_app_info)
      {
        g_debug ("Failed to detect app info for %s: %s",
                 sender, error->message);
        dex_promise_reject (app_info_promise, g_steal_pointer (&error));
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               XDG_DESKTOP_PORTAL_ERROR,
                                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Can't manually register unknown application");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    if (!xdp_app_info_is_host (detected_app_info))
      {
        const char *engine = xdp_app_info_get_engine_display_name (detected_app_info);

        g_debug ("Non-host (%s) sender %s tried to register a new app id",
                 engine, sender);
        dex_promise_reject (app_info_promise,
                            g_error_new_literal (G_IO_ERROR,
                                                 G_IO_ERROR_PERMISSION_DENIED,
                                                 "Not a host app"));
        g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                               XDG_DESKTOP_PORTAL_ERROR,
                                               XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                               "Can't manually register a %s application",
                                               engine);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }
  }

  new_app_info = dex_await_object (xdp_app_info_new_for_registered (peer, arg_app_id), &error);
  if (!new_app_info)
    {
      g_debug ("Can't create registered app for %s: %s",
               sender, error->message);
      dex_promise_reject (app_info_promise,
                          g_error_new_literal (G_IO_ERROR,
                                               G_IO_ERROR_PERMISSION_DENIED,
                                               "Failed to create app info"));
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Can't create registered app");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* And finally we can resolve the promise and wait for it to get applied */
  dex_promise_resolve_object (app_info_promise, g_object_ref (new_app_info));
  success = dex_await_boolean (g_steal_pointer (&insert_future), &error);

  if (!success && error)
    {
      g_debug ("Can't create registered app for %s: %s",
               sender, error->message);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Can't register app");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!success)
    {
      g_debug ("Connection %s already associated with an application ID", sender);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Connection already associated with an application ID");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_debug ("Added registered host app %s", xdp_app_info_get_id (new_app_info));

  xdp_dbus_host_registry_complete_register (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
registry_iface_init (XdpDbusHostRegistryIface *iface)
{
  iface->handle_register = handle_register;
}

static void
registry_dispose (GObject *object)
{
  Registry *registry = (Registry *) object;

  g_clear_object (&registry->app_info_registry);

  G_OBJECT_CLASS (registry_parent_class)->dispose (object);
}

static void
registry_init (Registry *registry)
{
}

static void
registry_class_init (RegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = registry_dispose;
}

static Registry *
registry_new (XdpAppInfoRegistry *app_info_registry)
{
  Registry *registry;

  registry = g_object_new (registry_get_type (), NULL);
  registry->app_info_registry = g_object_ref (app_info_registry);

  xdp_dbus_host_registry_set_version (XDP_DBUS_HOST_REGISTRY (registry), 1);

  return registry;
}

void
init_registry (XdpContext *context)
{
  g_autoptr(Registry) registry = NULL;
  XdpAppInfoRegistry *app_info_registry =
    xdp_context_get_app_info_registry (context);

  registry = registry_new (app_info_registry);

  /* We must ensure that method dispatches which modify the XdpAppInfo do so
   * before any other method call is dispatched. We do this by running in a
   * fiber on the main thread, and inserting our modification operation before
   * we yield (to the dbus main loop or the fiber scheduler via dex_await).
   */
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&registry)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER |
                                      XDP_CONTEXT_EXPORT_FLAGS_SKIP_AUTH);
}

#if HAVE_VARLINK

#define VARLINK_INTERFACE "org.freedesktop.host.portal.Registry"

static const char varlink_interface_description[] =
  "# This simple interface lets applications register their connections and\n"
  "# associate it with an application ID that will be used in portals.\n"
  "#\n"
  "# Either Register or Claim must be the first call a connection makes\n"
  "interface " VARLINK_INTERFACE "\n"
  "\n"
  "# Register a connection and associate it with an application ID. The\n"
  "# application ID must be able to match the basename of a .desktop file that\n"
  "# describes the application, and will be used in portal APIs to associate a\n"
  "# portal action with an application.\n"
  "#\n"
  "# app_id_hint is honoured only for applications xdg-desktop-portal does not\n"
  "# identify as sandboxed, and ignored for the rest.\n"
  "#\n"
  "# Registering can only be done at most once; any subsequent call will result\n"
  "# in an error. Registering must be done before any portal method call;\n"
  "# registering after such a call will result in an error\n"
  "method Register(app_id_hint: string) -> (instance_id: string)\n"
  "\n"
  "# Joins an instance created by Register, sharing its application ID\n"
  "method Claim(instance_id: string) -> ()\n"
  "\n"
  "# The connection already registered or claimed an instance\n"
  "error AlreadyRegistered()\n"
  "\n"
  "# No such instance exists, or it belongs to another application\n"
  "error NoSuchInstance()\n"
  "\n"
  "# The portal could not work out what application is calling\n"
  "error CannotIdentifyApp()\n";

typedef struct
{
  /* Instances are borrowed; they live only as long as the connections
   * claiming them */
  GHashTable *instances;

  guint64 last_instance_id;
} VarlinkRegistry;

static void
varlink_registry_free (gpointer data)
{
  VarlinkRegistry *registry = data;

  g_clear_pointer (&registry->instances, g_hash_table_unref);
  g_free (registry);
}

/* Returns the id of a new instance claimed by @connection, its only owner */
static char *
create_instance (VarlinkRegistry      *registry,
                 XdpVarlinkConnection *connection,
                 XdpAppInfo           *app_info)
{
  g_autoptr(XdpVarlinkInstance) instance = NULL;
  g_autofree char *id = NULL;

  id = g_strdup_printf ("%" G_GUINT64_FORMAT, ++registry->last_instance_id);
  instance = xdp_varlink_instance_new (registry->instances, id, app_info);
  xdp_varlink_connection_claim (connection, instance);

  return g_steal_pointer (&id);
}

/* Sandboxed apps may only claim their own instance. Host apps share one
 * identity, since any of them may register under any app id */
static gboolean
may_claim (XdpAppInfo *claimer,
           XdpAppInfo *owner)
{
  if (xdp_app_info_is_host (owner))
    return xdp_app_info_is_host (claimer);

  return g_strcmp0 (xdp_app_info_get_engine (claimer),
                    xdp_app_info_get_engine (owner)) == 0 &&
         g_strcmp0 (xdp_app_info_get_id (claimer),
                    xdp_app_info_get_id (owner)) == 0 &&
         g_strcmp0 (xdp_app_info_get_instance (claimer),
                    xdp_app_info_get_instance (owner)) == 0;
}

static gboolean
associate_app_info (XdpVarlinkService    *service,
                    XdpVarlinkConnection *connection,
                    XdpAppInfo           *app_info)
{
  XdpAppInfoRegistry *app_info_registry =
    xdp_varlink_service_get_app_info_registry (service);
  g_autoptr(DexFuture) insert_future = NULL;

  insert_future =
    xdp_app_info_registry_insert_future (app_info_registry,
                                         xdp_varlink_connection_get_peer (connection),
                                         dex_future_new_for_object (app_info));

  return dex_await_boolean (g_steal_pointer (&insert_future), NULL);
}

static long
handle_varlink_register (XdpVarlinkService    *service,
                         XdpVarlinkConnection *connection,
                         VarlinkCall          *call,
                         VarlinkObject        *parameters,
                         uint64_t              flags,
                         gpointer              user_data)
{
  VarlinkRegistry *registry = user_data;
  XdpPeer *peer = xdp_varlink_connection_get_peer (connection);
  g_autoptr(XdpAppInfo) detected_app_info = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autofree char *instance_id = NULL;
  g_autoptr(GError) error = NULL;
  const char *app_id_hint;

  if (xdp_varlink_connection_get_instance (connection) != NULL)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".AlreadyRegistered", NULL);

  if (varlink_object_get_string (parameters, "app_id_hint", &app_id_hint) < 0)
    return varlink_call_reply_invalid_parameter (call, "app_id_hint");

  detected_app_info = dex_await_object (xdp_app_info_new_for_peer (peer), &error);
  if (!detected_app_info)
    {
      g_debug ("Failed to detect app info for %s: %s",
               xdp_peer_get_key (peer), error->message);
      return varlink_call_reply_error (call, VARLINK_INTERFACE ".CannotIdentifyApp", NULL);
    }

  if (app_id_hint[0] != '\0' && xdp_app_info_is_host (detected_app_info))
    {
      app_info = dex_await_object (xdp_app_info_new_for_registered (peer, app_id_hint),
                                   &error);
      if (!app_info)
        {
          g_debug ("Rejected app id hint %s from %s: %s",
                   app_id_hint, xdp_peer_get_key (peer), error->message);
          return varlink_call_reply_invalid_parameter (call, "app_id_hint");
        }
    }
  else
    {
      if (app_id_hint[0] != '\0')
        g_debug ("Ignoring app id hint %s from %s application",
                 app_id_hint,
                 xdp_app_info_get_engine_display_name (detected_app_info));

      app_info = g_steal_pointer (&detected_app_info);
    }

  if (!associate_app_info (service, connection, app_info))
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".AlreadyRegistered", NULL);

  instance_id = create_instance (registry, connection, app_info);

  g_debug ("Registered %s as instance %s",
           xdp_app_info_get_id (app_info), instance_id);

  varlink_object_new (&reply);
  varlink_object_set_string (reply, "instance_id", instance_id);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_claim (XdpVarlinkService    *service,
                      XdpVarlinkConnection *connection,
                      VarlinkCall          *call,
                      VarlinkObject        *parameters,
                      uint64_t              flags,
                      gpointer              user_data)
{
  VarlinkRegistry *registry = user_data;
  XdpPeer *peer = xdp_varlink_connection_get_peer (connection);
  g_autoptr(XdpVarlinkInstance) instance = NULL;
  g_autoptr(XdpAppInfo) detected_app_info = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(GError) error = NULL;
  XdpVarlinkInstance *found;
  XdpAppInfo *app_info;
  const char *instance_id;

  if (xdp_varlink_connection_get_instance (connection) != NULL)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".AlreadyRegistered", NULL);

  if (varlink_object_get_string (parameters, "instance_id", &instance_id) < 0)
    return varlink_call_reply_invalid_parameter (call, "instance_id");

  found = g_hash_table_lookup (registry->instances, instance_id);
  if (!found)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSuchInstance", NULL);

  /* Only borrowed from the table, and we are about to await */
  instance = g_object_ref (found);

  detected_app_info = dex_await_object (xdp_app_info_new_for_peer (peer), &error);
  if (!detected_app_info)
    {
      g_debug ("Failed to detect app info for %s: %s",
               xdp_peer_get_key (peer), error->message);
      return varlink_call_reply_error (call, VARLINK_INTERFACE ".CannotIdentifyApp", NULL);
    }

  app_info = xdp_varlink_instance_get_app_info (instance);

  /* Missing rather than denied, so another app's instance is not observable */
  if (!may_claim (detected_app_info, app_info))
    {
      g_debug ("%s may not claim instance %s of %s",
               xdp_peer_get_key (peer), instance_id,
               xdp_app_info_get_id (app_info));
      return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSuchInstance", NULL);
    }

  if (!associate_app_info (service, connection, app_info))
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".AlreadyRegistered", NULL);

  xdp_varlink_connection_claim (connection, instance);

  g_debug ("Claimed instance %s for %s", instance_id, xdp_peer_get_key (peer));

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

static const XdpVarlinkMethod varlink_methods[] = {
  { "Register", handle_varlink_register, XDP_VARLINK_METHOD_FLAGS_PRE_CLAIM },
  { "Claim", handle_varlink_claim, XDP_VARLINK_METHOD_FLAGS_PRE_CLAIM },
};

gboolean
init_registry_varlink (XdpVarlinkService  *service,
                       GError            **error)
{
  VarlinkRegistry *registry;

  registry = g_new0 (VarlinkRegistry, 1);
  registry->instances = g_hash_table_new (g_str_hash, g_str_equal);

  return xdp_varlink_service_add_interface (service,
                                            varlink_interface_description,
                                            varlink_methods,
                                            G_N_ELEMENTS (varlink_methods),
                                            registry,
                                            varlink_registry_free,
                                            error);
}

#endif /* HAVE_VARLINK */
