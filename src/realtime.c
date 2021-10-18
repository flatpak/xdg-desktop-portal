/*
 * Copyright © 2021 Igalia S.L.
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
 *       Patrick Griffis <pgriffis@igalia.com>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "realtime.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "realtime"
#define PERMISSION_ID "realtime"

typedef struct _Realtime Realtime;
typedef struct _RealtimeClass RealtimeClass;

struct _Realtime
{
  XdpRealtimeSkeleton parent_instance;
  GDBusProxy *rtkit_proxy;
};

struct _RealtimeClass
{
  XdpRealtimeSkeletonClass parent_class;
};

static Realtime *realtime;

GType realtime_get_type (void) G_GNUC_CONST;
static void realtime_iface_init (XdpRealtimeIface *iface);

G_DEFINE_TYPE_WITH_CODE (Realtime, realtime, XDP_TYPE_REALTIME_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_REALTIME, realtime_iface_init));

static gboolean
map_pid_if_needed (XdpAppInfo *app_info, pid_t *pid, GError **error)
{
  if (!xdp_app_info_is_host (app_info))
  {
    if (!xdg_app_info_map_pids (app_info, pid, 1, error))
    {
      g_prefix_error (error, "Could not map pid: ");
      g_warning ("Realtime error: %s", (*error)->message);
      return FALSE;
    }
  }

  return TRUE;
}

static void
on_call_ready (GObject      *source_object,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&user_data);
  g_autoptr(GVariant) response = NULL;

  response = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                       result,
                                       &error);

  if (error)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static gboolean
handle_make_thread_realtime_with_pid (XdpRealtime           *object,
                                      GDBusMethodInvocation *invocation,
                                      guint64                process,
                                      guint64                thread,
                                      guint32                priority)
{
  g_autoptr (GError) error = NULL;
  Request *request = request_from_invocation (invocation);
  pid_t pids[1] = { process };
  const char *app_id = xdp_app_info_get_id (request->app_info);
  Permission permission;

  if (!realtime->rtkit_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "RealtimeKit was not found");
    }

  permission = get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);
  if (permission == PERMISSION_NO)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Permission denied");
      return TRUE;
    }

  if (!map_pid_if_needed (request->app_info, pids, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_dbus_proxy_call (G_DBUS_PROXY (realtime->rtkit_proxy),
                     "MakeThreadRealtimeWithPID",
                     g_variant_new ("(ttu)", pids[0], thread, priority),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     on_call_ready,
                     g_object_ref (invocation));

  return TRUE;
}

static gboolean
handle_make_thread_high_priority_with_pid (XdpRealtime           *object,
                                           GDBusMethodInvocation *invocation,
                                           guint64                process,
                                           guint64                thread,
                                           gint32                 priority)
{
  g_autoptr (GError) error = NULL;
  Request *request = request_from_invocation (invocation);
  pid_t pids[1] = { process };
  const char *app_id = xdp_app_info_get_id (request->app_info);
  Permission permission;

  if (!realtime->rtkit_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "RealtimeKit was not found");
    }

  permission = get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);
  if (permission == PERMISSION_NO)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Permission denied");
      return TRUE;
    }

  if (!map_pid_if_needed (request->app_info, pids, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_dbus_proxy_call (G_DBUS_PROXY (realtime->rtkit_proxy),
                     "MakeThreadRealtimeWithPID",
                     g_variant_new ("(ttu)", pids[0], thread, priority),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     on_call_ready,
                     g_object_ref (invocation));

  return TRUE;
}

static void
realtime_iface_init (XdpRealtimeIface *iface)
{
  iface->handle_make_thread_realtime_with_pid = handle_make_thread_realtime_with_pid;
  iface->handle_make_thread_high_priority_with_pid = handle_make_thread_high_priority_with_pid;
}

static void
realtime_init (Realtime *self)
{
  xdp_realtime_set_version (XDP_REALTIME (self), 1);
}

static void
realtime_finalize (GObject *object)
{
  Realtime *self = (Realtime *) object;

  g_clear_object (&self->rtkit_proxy);

  G_OBJECT_CLASS (realtime_parent_class)->finalize (object);
}

static void
realtime_class_init (RealtimeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = realtime_finalize;
}

static void
load_all_properties (GDBusProxy *proxy)
{
  const char * properties[] = { "MaxRealtimePriority", "MinNiceLevel", "RTTimeUSecMax" };
  enum prop_type { MAX_REALTIME_PRIORITY, MIN_NICE_LEVEL, RTTIME_USEC_MAX };

  for (guint i = 0; i < G_N_ELEMENTS (properties); ++i)
    {
      GVariant *result;
      GVariant *parameters;
      GError *error = NULL;

      parameters = g_variant_new ("(ss)", "org.freedesktop.RealtimeKit1", properties[i]);
      result = g_dbus_proxy_call_sync (proxy,
                                       "org.freedesktop.DBus.Properties.Get",
                                        g_steal_pointer (&parameters),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        NULL,
                                        &error);

      if (error)
        {
          g_warning ("Failed to load RealtimeKit property: %s", error->message);
          g_error_free (error);
        }
      else
        {
          GVariant *value;
          g_variant_get (result, "(v)", &value);

          if (i == MAX_REALTIME_PRIORITY)
            xdp_realtime_set_max_realtime_priority (XDP_REALTIME (realtime), g_variant_get_int32 (value));
          else if (i == MIN_NICE_LEVEL)
            xdp_realtime_set_min_nice_level (XDP_REALTIME (realtime), g_variant_get_int32 (value));
          else if (i == RTTIME_USEC_MAX)
            xdp_realtime_set_rttime_usec_max (XDP_REALTIME (realtime), g_variant_get_int64 (value));
          else
            g_assert_not_reached ();

          g_dbus_proxy_set_cached_property (proxy, properties[i], value);
          g_variant_unref (value);
          g_variant_unref (result);
        }
    }
}

GDBusInterfaceSkeleton *
realtime_create (GDBusConnection *connection)
{
  GDBusProxy *rtkit_proxy = NULL;
  g_autoptr (GError) error = NULL;

  rtkit_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               "org.freedesktop.RealtimeKit1",
                                               "/org/freedesktop/RealtimeKit1",
                                               "org.freedesktop.RealtimeKit1",
                                               NULL,
                                               &error);
  if (!rtkit_proxy)
    {
      /* We continue on so the realtime interface remains exported,
       * however it will fail to do anything */
      g_warning ("Failed to create RealtimeKit proxy: %s", error->message);
    }

  realtime = g_object_new (realtime_get_type (), NULL);
  realtime->rtkit_proxy = g_steal_pointer (&rtkit_proxy);

  if (realtime->rtkit_proxy)
    load_all_properties (realtime->rtkit_proxy);

  return G_DBUS_INTERFACE_SKELETON (realtime);
}
