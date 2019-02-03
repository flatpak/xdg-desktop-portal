/*
 * Copyright © 2019 Red Hat, Inc
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

#include "background.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "background"
#define PERMISSION_ID "background"

typedef struct _Background Background;
typedef struct _BackgroundClass BackgroundClass;

struct _Background
{
  XdpBackgroundSkeleton parent_instance;
};

struct _BackgroundClass
{
  XdpBackgroundSkeletonClass parent_class;
};

static XdpImplAccess *access_impl;
static Background *background;

GType background_get_type (void) G_GNUC_CONST;
static void background_iface_init (XdpBackgroundIface *iface);

G_DEFINE_TYPE_WITH_CODE (Background, background, XDP_TYPE_BACKGROUND_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_BACKGROUND, background_iface_init));


typedef enum { UNSET, NO, YES, ASK } Permission;

static Permission
get_permission (const char *app_id)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  const char **permissions;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSION_TABLE,
                                                   PERMISSION_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No background permissions found: %s", error->message);
      return UNSET;
    }

  if (!g_variant_lookup (out_perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No permissions stored for: app %s", app_id);

      return UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
      return UNSET;
    }
  g_debug ("permission store: app %s -> %s", app_id, permissions[0]);

  if (strcmp (permissions[0], "yes") == 0)
    return YES;
  else if (strcmp (permissions[0], "no") == 0)
    return NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return UNSET;
}

static void
set_permission (const char *app_id,
                Permission permission)
{
  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  if (permission == ASK)
    permissions[0] = "ask";
  else if (permission == YES)
    permissions[0] = "yes";
  else if (permission == NO)
    permissions[0] = "no";
  else
    {
      g_warning ("Wrong permission format, ignoring");
      return;
    }
  permissions[1] = NULL;

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           PERMISSION_TABLE,
                                                           TRUE,
                                                           PERMISSION_ID,
                                                           app_id,
                                                           (const char * const*)permissions,
                                                           NULL,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error updating permission store: %s", error->message);
    }
}

static void
handle_request_background_in_thread_func (GTask *task,
                                          gpointer source_object,
                                          gpointer task_data,
                                          GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  GVariant *options;
  const char *app_id;
  Permission permission;
  gboolean allowed;
  const char *reason = NULL;

  REQUEST_AUTOLOCK (request);

  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");
  g_variant_lookup (options, "reason", "s", &reason);

  app_id = xdp_app_info_get_id (request->app_info);
  permission = get_permission (app_id);

  g_debug ("Handle RequestBackground for %s\n", app_id);

  if (permission == ASK || permission == UNSET)
    {
      GVariantBuilder opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GAppInfo) info = NULL;

      if (app_id[0] != 0)
        {
          g_autofree char *desktop_id;
          desktop_id = g_strconcat (app_id, ".desktop", NULL);
          info = (GAppInfo*)g_desktop_app_info_new (desktop_id);
        }

      title = g_strdup_printf (_("Allow %s to run in the background?"), info ? g_app_info_get_display_name (info) : app_id);
      if (reason)
        subtitle = g_strdup (reason);
      else
        subtitle = g_strdup_printf (_("%s requests to run in the background."), info ? g_app_info_get_display_name (info) : app_id);
      body = g_strdup (_("The ‘run in background’ permission can be changed at any time from the application settings."));

      g_debug ("Calling backend for background access for: %s", app_id);

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&opt_builder, "{sv}", "deny_label", g_variant_new_string (_("Don't allow")));
      g_variant_builder_add (&opt_builder, "{sv}", "grant_label", g_variant_new_string (_("Allow")));
      if (!xdp_impl_access_call_access_dialog_sync (access_impl,
                                                    request->id,
                                                    app_id,
                                                    "",
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&opt_builder),
                                                    &response,
                                                    &results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("AccessDialog call failed: %s", error->message);
          g_clear_error (&error);
        }

      allowed = response == 0;

      if (permission == UNSET)
        set_permission (app_id, allowed ? YES : NO);
    }
  else
    allowed = permission == YES ? TRUE : FALSE;

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&results, "{sv}", "background", g_variant_new_boolean (allowed));
      xdp_request_emit_response (XDP_REQUEST (request),
                                 allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
validate_reason (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (g_utf8_strlen (string, -1) > 256)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting overly long reasons");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey background_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason }
};

static gboolean
handle_request_background (XdpBackground *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_window,
                           GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;
  GVariantBuilder opt_builder;
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &opt_builder,
                      background_options, G_N_ELEMENTS (background_options),
                      NULL);

  options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (access_impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (access_impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_background_complete_request_background (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_request_background_in_thread_func);

  return TRUE;
}

static void
background_iface_init (XdpBackgroundIface *iface)
{
  iface->handle_request_background = handle_request_background;
}

static void
background_init (Background *background)
{
  xdp_background_set_version (XDP_BACKGROUND (background), 1);
}

static void
background_class_init (BackgroundClass *klass)
{
}

GDBusInterfaceSkeleton *
background_create (GDBusConnection *connection,
                   const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  access_impl = xdp_impl_access_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                dbus_name,
                                                DESKTOP_PORTAL_OBJECT_PATH,
                                                NULL,
                                                &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  background = g_object_new (background_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (background);
}
