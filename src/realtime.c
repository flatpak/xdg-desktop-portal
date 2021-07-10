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
#include "xdp-dbus.h"
#include "xdp-utils.h"

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
on_get_property_ready (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&user_data);
  g_autoptr(GVariant) response = NULL;
  gint64 ret;

  response = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                       result,
                                       &error);

  if (error)
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    return;
  }

  if (error)
    g_warning ("Realtime error getting property: %s", error->message);
  else
  {
    g_autoptr(GVariant) child = NULL;
    g_variant_get_child (response, 0, "v", &child);

    if (g_variant_is_of_type (child, G_VARIANT_TYPE_INT64))
      ret = g_variant_get_int64 (child);
    else if (g_variant_is_of_type (child, G_VARIANT_TYPE_INT32))
      ret = g_variant_get_int32 (child);
    else
    {
      error = g_error_new_literal (XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED, "Invalid response type recieved");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }
  }

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", ret));
}

static gboolean
handle_get_property (XdpRealtime           *object,
                     GDBusMethodInvocation *invocation,
                     const char            *property_name)
{
  g_dbus_proxy_call (G_DBUS_PROXY (realtime->rtkit_proxy),
                     "org.freedesktop.DBus.Properties.Get",
                     g_variant_new ("(ss)", "org.freedesktop.RealtimeKit1", property_name),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     on_get_property_ready,
                     g_object_ref (invocation));

  return TRUE;
}

static void
realtime_iface_init (XdpRealtimeIface *iface)
{
  iface->handle_make_thread_realtime_with_pid = handle_make_thread_realtime_with_pid;
  iface->handle_make_thread_high_priority_with_pid = handle_make_thread_high_priority_with_pid;
  iface->handle_get_property = handle_get_property;
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

GDBusInterfaceSkeleton *
realtime_create (GDBusConnection *connection)
{
  g_autoptr(GDBusConnection) system_bus = NULL;
  GDBusProxy *rtkit_proxy;
  g_autoptr (GError) error = NULL;

  system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);

  if (system_bus == NULL)
  {
    g_warning ("Failed to connect to system bus for RealtimeKit: %s", error->message);
    return NULL;
  }

  rtkit_proxy = g_dbus_proxy_new_sync (system_bus,
                                       G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION |
                                        G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS |
                                        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                       NULL,
                                       "org.freedesktop.RealtimeKit1",
                                       "/org/freedesktop/RealtimeKit1",
                                       "org.freedesktop.RealtimeKit1",
                                       NULL,
                                       &error);

  if (rtkit_proxy == NULL)
    {
      g_warning ("Failed to create RealtimeKit proxy: %s", error->message);
      return NULL;
    }

  realtime = g_object_new (realtime_get_type (), NULL);
  realtime->rtkit_proxy = g_steal_pointer (&rtkit_proxy);

  return G_DBUS_INTERFACE_SKELETON (realtime);
}
