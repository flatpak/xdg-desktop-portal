/*
 * Copyright © 2018 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "location.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"
#include "session.h"
#include "geoclue-dbus.h"
#include <geoclue.h>

static GClueAccuracyLevel gclue_accuracy_level_from_string (const char *str);
static const char *       gclue_accuracy_level_to_string   (GClueAccuracyLevel level);

static GQuark quark_request_session;
extern gboolean opt_verbose;

typedef enum {
  LOCATION_SESSION_STATE_INIT,
  LOCATION_SESSION_STATE_STARTING,
  LOCATION_SESSION_STATE_STARTED,
  LOCATION_SESSION_STATE_CLOSED
} LocationSessionState;

typedef struct
{
  Session parent;

  LocationSessionState state;

  guint distance_threshold;
  guint time_threshold;
  guint accuracy;

  GeoclueClient *client;
} LocationSession;

typedef struct
{
  SessionClass parent_class;
} LocationSessionClass;

GType location_session_get_type (void);

G_DEFINE_TYPE (LocationSession, location_session, session_get_type ())

static void
location_session_init (LocationSession *session)
{
  session->distance_threshold = 0;
  session->time_threshold = 0;
  session->accuracy = GCLUE_ACCURACY_LEVEL_EXACT;
}

static void
location_session_close (Session *session)
{
  LocationSession *loc_session = (LocationSession *)session;

  loc_session->state = LOCATION_SESSION_STATE_CLOSED;

  if (loc_session->client)
    geoclue_client_call_stop (loc_session->client, NULL, NULL, NULL);

  g_debug ("location session '%s' closed", session->id);
}

static void
location_session_finalize (GObject *object)
{
  LocationSession *loc_session = (LocationSession *)object;

  g_clear_object (&loc_session->client);

  G_OBJECT_CLASS (location_session_parent_class)->finalize (object);
}

static void
location_session_class_init (LocationSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  SessionClass *session_class = (SessionClass *)klass;

  object_class->finalize = location_session_finalize;

  session_class->close = location_session_close;
}

static LocationSession *
location_session_new (GVariant *options,
                      GDBusMethodInvocation *invocation,
                      GError **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  XdpAppInfo *app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, NULL);
  Session *session;

  session = g_initable_new (location_session_get_type (), NULL, error,
                            "sender", sender,
                            "app-id", xdp_app_info_get_id (app_info),
                            "token", lookup_session_token (options),
                            "connection", connection,
                            NULL);

  if (session)
    g_debug ("location session '%s' created", session->id);

  return (LocationSession*)session;
}

/*** GeoClue integration ***/

static void
location_updated (GeoclueClient *client,
                  const char *old_location,
                  const char *new_location,
                  gpointer data)
{
  Session *session = data;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) dict = NULL;

  g_debug ("GeoClue client ::LocationUpdated %s -> %s\n",  old_location, new_location);

  if (strcmp (new_location, "/") == 0)
    return;

  ret = g_dbus_connection_call_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (client)),
                                     "org.freedesktop.GeoClue2",
                                     new_location,
                                     "org.freedesktop.DBus.Properties",
                                     "GetAll",
                                     g_variant_new ("(s)", "org.freedesktop.GeoClue2.Location"),
                                     G_VARIANT_TYPE ("(a{sv})"),
                                     0, -1, NULL, &error);
  if (ret == NULL)
    {
      g_warning ("Failed to get location properties: %s", error->message);
      return;
    }

  g_variant_get (ret, "(@a{sv})", &dict);

  if (opt_verbose)
    {
      g_autofree char *a = g_variant_print (dict, FALSE);
      g_debug ("location data: %s\n", a);
    }

  if (!g_dbus_connection_emit_signal (session->connection,
                                      session->sender,
                                      "/org/freedesktop/portal/desktop",
                                      "org.freedesktop.portal.Location",
                                      "LocationUpdated",
                                      g_variant_new ("(o@a{sv})", session->id, dict),
                                      &error))
    {
      g_warning ("Failed to emit LocationUpdated signal: %s", error->message);
    }
}

static gboolean
location_session_start (LocationSession *loc_session)
{
  g_autoptr(GDBusConnection) system_bus = NULL;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;
  char *client_id;

  /* FIXME: this is all ugly and sync */

  system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  ret = g_dbus_connection_call_sync (system_bus,
                                     "org.freedesktop.GeoClue2",
                                     "/org/freedesktop/GeoClue2/Manager",
                                     "org.freedesktop.GeoClue2.Manager",
                                     "GetClient",
                                     NULL,
                                     G_VARIANT_TYPE ("(o)"),
                                     0, -1, NULL, &error);
  if (ret == NULL)
    {
      g_warning ("Failed to get GeoClue client: %s", error->message);
      loc_session->state = LOCATION_SESSION_STATE_CLOSED;
      return FALSE;
    }

  g_variant_get (ret, "(&o)", &client_id);

  loc_session->client = geoclue_client_proxy_new_sync (system_bus,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       "org.freedesktop.GeoClue2",
                                                       client_id,
                                                       NULL,
                                                       &error);
  if (loc_session->client == NULL)
    {
      g_warning ("Failed to get GeoClue client: %s", error->message);
      loc_session->state = LOCATION_SESSION_STATE_CLOSED;
      return FALSE;
    }

  g_debug ("location session '%s', GeoClue client '%s'", ((Session*)loc_session)->id, client_id);
  g_debug ("location session '%s', distance-threshold %d, time-threshold %d, accuracy %s",
           ((Session *)loc_session)->id,
           loc_session->distance_threshold,
           loc_session->time_threshold,
           gclue_accuracy_level_to_string (loc_session->accuracy));
  
  g_object_set (loc_session->client,
                "desktop-id", "xdg-desktop-portal",
                "distance-threshold", loc_session->distance_threshold,
                "time-threshold", loc_session->time_threshold,
                "requested-accuracy-level", loc_session->accuracy,
                NULL);
  
  g_signal_connect (loc_session->client, "location-updated",
                    G_CALLBACK (location_updated), loc_session);

  if (!geoclue_client_call_start_sync (loc_session->client, NULL, &error))
    {
      g_warning ("Starting GeoClue client failed: %s", error->message);
      loc_session->state = LOCATION_SESSION_STATE_CLOSED;
      g_clear_object (&loc_session->client);
      return FALSE;
    }

  g_debug ("GeoClue client '%s' started", client_id);

  loc_session->state = LOCATION_SESSION_STATE_STARTED;
  g_debug ("location session '%s' started", ((Session*)loc_session)->id);

  return TRUE;
}

/*** Permission handling ***/

/* We use a table named 'location' with a single row with ID 'location'.
 * The permissions string for each application entry consists of
 * the allowed accuracy and the last-use timestamp (using monotonic time)
 * Example:
 *
 * location
 *   location
 *     org.gnome.PortalTest   CITY,1234131441
 *     org.gnome.Todo         EXACT,00909313134
 *     org.gnome.Polari       NONE,0
 *
 * When no entry is found, we ask the user whether he wants to grant
 * access, and use EXACT as the accuracy.
 */

#define PERMISSION_TABLE "location"
#define PERMISSION_ID "location"

static struct { const char *name; GClueAccuracyLevel level; } accuracy_levels[] = {
  { "NONE", GCLUE_ACCURACY_LEVEL_NONE },
  { "COUNTRY", GCLUE_ACCURACY_LEVEL_COUNTRY },
  { "CITY", GCLUE_ACCURACY_LEVEL_CITY },
  { "NEIGHBORHOOD", GCLUE_ACCURACY_LEVEL_NEIGHBORHOOD },
  { "STREET", GCLUE_ACCURACY_LEVEL_STREET },
  { "EXACT", GCLUE_ACCURACY_LEVEL_EXACT }
};

static GClueAccuracyLevel
gclue_accuracy_level_from_string (const char *str)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (accuracy_levels); i++)
    {
      if (g_str_equal (accuracy_levels[i].name, str))
        return accuracy_levels[i].level;
    }

  g_warning ("Unknown accuracy level: %s", str);
  return GCLUE_ACCURACY_LEVEL_NONE;
}

static const char *
gclue_accuracy_level_to_string (GClueAccuracyLevel level)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (accuracy_levels); i++)
    {
      if (accuracy_levels[i].level == level)
        return accuracy_levels[i].name;
    }

  g_warning ("Unknown accuracy level: %d", level);
  return "NONE";
}

static gboolean
get_location_permissions (XdpAppInfo *app_info,
                          GClueAccuracyLevel *accuracy,
                          gint64 *last_used)
{
  const char *app_id = xdp_app_info_get_id (app_info);
  g_auto(GStrv) perms = NULL;

  if (xdp_app_info_is_host (app_info))
    {
      /* unsandboxed */
      *accuracy = GCLUE_ACCURACY_LEVEL_EXACT;
      *last_used = 0;
      return TRUE;
    }

  g_debug ("Getting location permissions for '%s'", app_id);

  perms = get_permissions_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);

  if (perms == NULL)
    return FALSE;

  if (g_strv_length ((char **)perms) < 2)
    {
      g_warning ("Wrong permission format");
      return FALSE;
    }

  *accuracy = gclue_accuracy_level_from_string (perms[0]);
  *last_used = g_ascii_strtoll (perms[1], NULL, 10);

  g_debug ("got permission store accuracy: %s -> %d", perms[0], *accuracy);

  return TRUE;
}

static void
set_location_permissions (const char *app_id,
                          GClueAccuracyLevel accuracy,
                          gint64 timestamp)
{
  g_autofree char *date = NULL;
  const char *permissions[3];

  if (app_id == NULL)
    return;

  date = g_strdup_printf ("%" G_GINT64_FORMAT, timestamp);
  permissions[0] = gclue_accuracy_level_to_string (accuracy);
  permissions[1] = (const char *)date;
  permissions[2] = NULL;

  g_debug ("set permission store accuracy: %d -> %s", accuracy, permissions[0]);

  set_permissions_sync (app_id, PERMISSION_TABLE, PERMISSION_ID, permissions);
}

/*** Location boilerplace ***/

typedef struct
{
  XdpDbusLocationSkeleton parent_instance;
} Location;

typedef struct 
{
  XdpDbusLocationSkeletonClass parent_class;
} LocationClass;

static Location *location;
static XdpDbusImplAccess *access_impl;
static XdpDbusImplLockdown *lockdown;

GType location_get_type (void) G_GNUC_CONST;
static void location_iface_init (XdpDbusLocationIface *iface);

G_DEFINE_TYPE_WITH_CODE (Location, location, XDP_DBUS_TYPE_LOCATION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_LOCATION, location_iface_init))

/*** CreateSession ***/

static gboolean
handle_create_session (XdpDbusLocation *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  g_autoptr(GError) error = NULL;
  LocationSession *session;
  guint threshold;
  guint accuracy;

  if (xdp_dbus_impl_lockdown_get_disable_location (lockdown))
    {
      g_debug ("Location services disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Location services disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = location_session_new (arg_options, invocation, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (g_variant_lookup (arg_options, "distance-threshold", "u", &threshold))
    session->distance_threshold = threshold;
  if (g_variant_lookup (arg_options, "time-threshold", "u", &threshold))
    session->time_threshold = threshold;
  if (g_variant_lookup (arg_options, "accuracy", "u", &accuracy))
    {
      if (accuracy == 0)
        session->accuracy = GCLUE_ACCURACY_LEVEL_NONE;
      else if (accuracy == 1)
        session->accuracy = GCLUE_ACCURACY_LEVEL_COUNTRY;
      else if (accuracy == 2)
        session->accuracy = GCLUE_ACCURACY_LEVEL_CITY;
      else if (accuracy == 3)
        session->accuracy = GCLUE_ACCURACY_LEVEL_NEIGHBORHOOD;
      else if (accuracy == 4)
        session->accuracy = GCLUE_ACCURACY_LEVEL_STREET;
      else if (accuracy == 5)
        session->accuracy = GCLUE_ACCURACY_LEVEL_EXACT;
      else
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 "Invalid accuracy level");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  if (!session_export ((Session *)session, &error))
    {
       g_warning ("Failed to export session: %s", error->message);
       session_close ((Session *)session, FALSE);
    }
  else
    {
      g_debug ("CreateSession new session '%s'",  ((Session *)session)->id);
      session_register ((Session *)session);
    }

  xdp_dbus_location_complete_create_session (object, invocation, ((Session *)session)->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/*** Start ***/

static void
handle_start_in_thread_func (GTask *task,
                             gpointer source_object,
                             gpointer task_data,
                             GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *parent_window;
  const char *id;
  gint64 last_used = 0;
  g_autoptr(GError) error = NULL;
  guint response = 2;
  Session *session;
  LocationSession *loc_session;
  GClueAccuracyLevel accuracy;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_qdata (G_OBJECT (request), quark_request_session);
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);
  loc_session = (LocationSession *)session;

  parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");

  id = xdp_app_info_get_id (request->app_info);

  if (!get_location_permissions (request->app_info, &accuracy, &last_used))
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      g_autoptr(XdpDbusImplRequest) impl_request = NULL;
      GVariantBuilder access_opt_builder;
      g_autofree char *app_id = NULL;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      const char *body;

      impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (access_impl)),
                                                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                           g_dbus_proxy_get_name (G_DBUS_PROXY (access_impl)),
                                                           request->id,
                                                           NULL, NULL);

      request_set_impl_request (request, impl_request);

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny Access")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Grant Access")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("find-location-symbolic"));

      if (g_strcmp0 (id, "") != 0)
        {
          g_autoptr(GAppInfo) info = NULL;
          const gchar *name = NULL;

          info = xdp_app_info_load_app_info (request->app_info);

          if (info)
            {
              name = g_app_info_get_display_name (G_APP_INFO (info));
              app_id = xdp_get_app_id_from_desktop_id (g_app_info_get_id (info));
            }
          else
            {
              name = app_id;
              app_id = g_strdup (id);
            }

          title = g_strdup_printf (_("Give %s Access to Your Location?"), name);

          if (info && g_desktop_app_info_has_key (G_DESKTOP_APP_INFO (info), "X-Geoclue-Reason"))
            subtitle = g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (info), "X-Geoclue-Reason");
          else
            subtitle = g_strdup_printf (_("%s wants to use your location."), name);
        }
      else
        {
          /* Note: this will set the location permission for all unsandboxed
           * apps for which an app ID can't be determined.
           */
          g_assert (xdp_app_info_is_host (request->app_info));
          app_id = g_strdup ("");
          title = g_strdup (_("Grant Access to Your Location?"));
          subtitle = g_strdup (_("An app wants to use your location."));
        }

      body = _("Location access can be changed at any time from the privacy settings.");

      if (!xdp_dbus_impl_access_call_access_dialog_sync (access_impl,
                                                         request->id,
                                                         app_id,
                                                         parent_window,
                                                         title,
                                                         subtitle,
                                                         body,
                                                         g_variant_builder_end (&access_opt_builder),
                                                         &access_response,
                                                         &access_results,
                                                         NULL,
                                                         &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);
          goto out;
        }

      request_set_impl_request (request, NULL);

      accuracy = (access_response == 0) ? GCLUE_ACCURACY_LEVEL_EXACT : GCLUE_ACCURACY_LEVEL_NONE;
    }

  if (accuracy != GCLUE_ACCURACY_LEVEL_NONE)
    last_used = g_get_monotonic_time ();

  set_location_permissions (id, accuracy, last_used);

  if (accuracy == GCLUE_ACCURACY_LEVEL_NONE)
    {
      response = 1;
      goto out;
    }

  if (accuracy < loc_session->accuracy)
    {
      g_debug ("Lowering requested accuracy from %s to %s",
               gclue_accuracy_level_to_string (loc_session->accuracy),
               gclue_accuracy_level_to_string (accuracy));
      loc_session->accuracy = accuracy;
    }

  if (location_session_start ((LocationSession*)session))
    response = 0;
  else
    response = 2;

out:
  if (request->exported)
    {
      GVariantBuilder opt_builder;

      g_debug ("sending response: %d", response);
      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }  

  if (response != 0)
    {
       g_debug ("closing session");
       session_close ((Session *)session, FALSE);
    }
}

static gboolean
handle_start (XdpDbusLocation *object,
              GDBusMethodInvocation *invocation,
              const char *arg_session_handle,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  LocationSession *loc_session;
  g_autoptr(GTask) task = NULL;

  if (xdp_dbus_impl_lockdown_get_disable_location (lockdown))
    {
      g_debug ("Location services disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Location services disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);

  session = acquire_session (arg_session_handle, request);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  loc_session = (LocationSession *)session;
  switch (loc_session->state)
    {
    case LOCATION_SESSION_STATE_INIT:
      break;
    case LOCATION_SESSION_STATE_STARTING:
    case LOCATION_SESSION_STATE_STARTED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Can only start once");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    case LOCATION_SESSION_STATE_CLOSED:
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);

  g_object_set_qdata_full (G_OBJECT (request),
                           quark_request_session,
                           g_object_ref (session),
                           g_object_unref);

  loc_session->state = LOCATION_SESSION_STATE_STARTING;

  xdp_dbus_location_complete_start (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_start_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/************/

static void
location_iface_init (XdpDbusLocationIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_start = handle_start;
}

static void
location_init (Location *location)
{
  xdp_dbus_location_set_version (XDP_DBUS_LOCATION (location), 1);
}

static void
location_class_init (LocationClass *klass)
{
  quark_request_session = g_quark_from_static_string ("-xdp-request-location-session");
}

GDBusInterfaceSkeleton *
location_create (GDBusConnection *connection,
                 const char *dbus_name,
                 gpointer lockdown_proxy)
{
  g_autoptr(GError) error = NULL;

  lockdown = lockdown_proxy;

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL, &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  location = g_object_new (location_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (location);
}
