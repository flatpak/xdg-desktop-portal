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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "inhibit.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define TABLE_NAME "inhibit"

enum {
  INHIBIT_LOGOUT  = 1,
  INHIBIT_SWITCH  = 2,
  INHIBIT_SUSPEND = 4,
  INHIBIT_IDLE    = 8
};

#define INHIBIT_ALL (INHIBIT_LOGOUT|INHIBIT_SWITCH|INHIBIT_SUSPEND|INHIBIT_IDLE)

typedef struct _Inhibit Inhibit;
typedef struct _InhibitClass InhibitClass;

struct _Inhibit
{
  XdpInhibitSkeleton parent_instance;
};

struct _InhibitClass
{
  XdpInhibitSkeletonClass parent_class;
};

static XdpImplInhibit *impl;
static Inhibit *inhibit;

GType inhibit_get_type (void) G_GNUC_CONST;
static void inhibit_iface_init (XdpInhibitIface *iface);

G_DEFINE_TYPE_WITH_CODE (Inhibit, inhibit, XDP_TYPE_INHIBIT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_INHIBIT, inhibit_iface_init));

static void
inhibit_done (GObject *source,
              GAsyncResult *result,
              gpointer data)
{
  g_autoptr(Request) request = data;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_inhibit_call_inhibit_finish (impl, result, &error))
    g_warning ("Backend call failed: %s", error->message);
}

static guint32
get_allowed_inhibit (const char *app_id)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   "inhibit",
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error getting permissions: %s", error->message);
      g_clear_error (&error);
    }

  if (out_perms != NULL)
    {
      const char **perms;
      if (g_variant_lookup (out_perms, app_id, "^a&s", &perms))
        {
          guint32 ret = 0;
          int i;

          for (i = 0; perms[i]; i++)
            {
              if (strcmp (perms[i], "logout") == 0)
                ret |= INHIBIT_LOGOUT;
              else if (strcmp (perms[i], "switch") == 0)
                ret |= INHIBIT_SWITCH;
              else if (strcmp (perms[i], "suspend") == 0)
                ret |= INHIBIT_SUSPEND;
              else if (strcmp (perms[i], "idle") == 0)
                ret |= INHIBIT_IDLE;
              else
                g_warning ("Unknown inhibit flag in permission store: %s", perms[i]);
            }
            return ret;
        }
    }

  return INHIBIT_ALL; /* all allowed */
}

static void
handle_inhibit_in_thread_func (GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *window;
  guint32 flags;
  GVariant *options;

  REQUEST_AUTOLOCK (request);

  window = (const char *)g_object_get_data (G_OBJECT (request), "window");
  flags = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (request), "flags"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  flags = flags & get_allowed_inhibit (xdp_app_info_get_id (request->app_info));
  if (flags == 0)
    return;

  xdp_impl_inhibit_call_inhibit (impl,
                                 request->id,
                                 xdp_app_info_get_id (request->app_info),
                                 window,
                                 flags,
                                 options,
                                 NULL,
                                 inhibit_done,
                                 g_object_ref (request));
}

static gboolean
inhibit_handle_inhibit (XdpInhibit *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_window,
                        guint32 arg_flags,
                        GVariant *options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;

  REQUEST_AUTOLOCK (request);

  if ((arg_flags & ~INHIBIT_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid flags");
      return TRUE;
    }

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);
  g_object_set_data (G_OBJECT (request), "flags", GUINT_TO_POINTER (arg_flags));
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_inhibit_in_thread_func);

  xdp_inhibit_complete_inhibit (object, invocation, request->id);

  return TRUE;
}

static void
inhibit_iface_init (XdpInhibitIface *iface)
{
  iface->handle_inhibit = inhibit_handle_inhibit;
}

static void
inhibit_init (Inhibit *inhibit)
{
  xdp_inhibit_set_version (XDP_INHIBIT (inhibit), 1);
}

static void
inhibit_class_init (InhibitClass *klass)
{
}

GDBusInterfaceSkeleton *
inhibit_create (GDBusConnection *connection,
                const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_inhibit_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          dbus_name,
                                          "/org/freedesktop/portal/desktop",
                                          NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create inhibit proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  inhibit = g_object_new (inhibit_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (inhibit);
}
