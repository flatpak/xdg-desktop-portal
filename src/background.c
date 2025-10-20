/*
 * Copyright © 2019 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#include "flatpak-instance.h"
#include "xdp-app-info.h"
#include "xdp-background-monitor.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-utils.h"

#include "background.h"

/* Implementation notes:
 *
 * We store a YES/NO/ASK permission for "run in background".
 *
 * There is a portal api for apps to request this permission
 * ahead of time. The portal also lets apps ask for being
 * autostarted.
 *
 * We determine this condition by getting per-application
 * state from the compositor, and comparing that list to
 * the list of running flatpak instances obtained from
 * $XDG_RUNTIME_DIR/.flatpak/. A thread is comparing
 * this list every minute, and if it finds an app that
 * is in the background twice, we take actions:
 * - if the permission is NO, we kill it
 * - if the permission is YES or ASK, we notify the user
 *
 * We only notify once per running instance to not be
 * annoying.
 *
 * Platform-dependent parts are in the background portal
 * backend:
 * - Notifying the user
 * - Getting compositor state
 * - Enable or disable autostart
 */

#define PERMISSION_TABLE "background"
#define PERMISSION_ID "background"

typedef struct _Background Background;
typedef struct _BackgroundClass BackgroundClass;

struct _Background
{
  XdpDbusBackgroundSkeleton parent_instance;

  XdpBackgroundMonitor *monitor;
};

struct _BackgroundClass
{
  XdpDbusBackgroundSkeletonClass parent_class;
};

static XdpDbusImplAccess *access_impl;
static XdpDbusImplBackground *background_impl;
static Background *background;
static GFileMonitor *instance_monitor;

GType background_get_type (void) G_GNUC_CONST;
static void background_iface_init (XdpDbusBackgroundIface *iface);

G_DEFINE_TYPE_WITH_CODE (Background, background,
                         XDP_DBUS_TYPE_BACKGROUND_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_BACKGROUND,
                                                background_iface_init));

typedef enum {
  BACKGROUND = 0,
  RUNNING = 1,
  ACTIVE = 2,
} AppState;

typedef enum {
  FORBID = 0,
  ALLOW  = 1,
  IGNORE = 2
} NotifyResult;

static GVariant *
get_all_permissions (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_dbus_impl_permission_store_call_lookup_sync (xdp_get_permission_store (),
                                                        PERMISSION_TABLE,
                                                        PERMISSION_ID,
                                                        &out_perms,
                                                        &out_data,
                                                        NULL,
                                                        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No background permissions found: %s", error->message);
      return NULL;
    }

  return g_steal_pointer (&out_perms);
}

static XdpPermission
get_one_permission (const char *app_id,
                    GVariant   *perms)
{
  g_autofree const char **permissions = NULL;

  if (perms == NULL)
    {
      g_debug ("No background permissions found");

      return XDP_PERMISSION_UNSET;
    }
  else if (!g_variant_lookup (perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No background permissions stored for: app %s", app_id);

      return XDP_PERMISSION_UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong background permission format, ignoring (%s)", a);
      return XDP_PERMISSION_UNSET;
    }

  g_debug ("permission store: background, app %s -> %s", app_id, permissions[0]);

  if (strcmp (permissions[0], "yes") == 0)
    return XDP_PERMISSION_YES;
  else if (strcmp (permissions[0], "no") == 0)
    return XDP_PERMISSION_NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return XDP_PERMISSION_ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return XDP_PERMISSION_UNSET;
}

static XdpPermission
get_permission (const char *app_id)
{
  g_autoptr(GVariant) perms = NULL;

  perms = get_all_permissions ();
  if (perms)
    return get_one_permission (app_id, perms);

  return XDP_PERMISSION_UNSET;
}

static void
set_permission (const char *app_id,
                XdpPermission permission)
{
  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  if (permission == XDP_PERMISSION_ASK)
    permissions[0] = "ask";
  else if (permission == XDP_PERMISSION_YES)
    permissions[0] = "yes";
  else if (permission == XDP_PERMISSION_NO)
    permissions[0] = "no";
  else
    {
      g_warning ("Wrong permission format, ignoring");
      return;
    }
  permissions[1] = NULL;

  if (!xdp_dbus_impl_permission_store_call_set_permission_sync (xdp_get_permission_store (),
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

/* background monitor */

/* The background monitor is running in a dedicated thread.
 *
 * We rely on the RunningApplicationsChanged signal from the backend to get
 * notified about applications that start or stop having open windows, and on
 * file monitoring to learn about flatpak instances appearing and disappearing.
 *
 * When either of these changes happens, we wake up the background monitor
 * thread, and it will check the state of applications a few times, with a
 * few seconds of wait in between. When we find an application in the background
 * more than once, we check the permissions, and kill or notify if warranted.
 *
 * We require an application to be in background state for more than once check
 * to avoid killing an unlucky application that just happened to start up as we
 * did our check.
 */

static GHashTable *
get_app_states (void)
{
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GHashTable) app_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *appid;
  GVariant *value;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_background_call_get_app_state_sync (background_impl, &apps, NULL, &error))
    {
      static int warned = 0;

      if (!warned)
        {
          g_warning ("Failed to get application states: %s", error->message);
          warned = 1;
        }

      return NULL;
    }

  g_autoptr(GVariantIter) iter = g_variant_iter_new (apps);
  while (g_variant_iter_loop (iter, "{&sv}", &appid, &value))
    {
      AppState state = g_variant_get_uint32 (value);
      g_hash_table_insert (app_states, g_strdup (appid), GINT_TO_POINTER (state));
    }

  return g_steal_pointer (&app_states);
}

static AppState
get_one_app_state (const char *app_id,
                   GHashTable *app_states)
{
  return (AppState)GPOINTER_TO_INT (g_hash_table_lookup (app_states, app_id));
}

typedef struct {
  FlatpakInstance *instance;
  int stamp;
  AppState state;
  char *handle;
  gboolean notified;
  XdpPermission permission;
  char *status_message;
} InstanceData;

static void
instance_data_free (gpointer data)
{
  InstanceData *idata = data;

  g_object_unref (idata->instance);
  g_free (idata->status_message);
  g_free (idata->handle);

  g_free (idata);
}

/* instance ID -> InstanceData
 */
static GHashTable *applications;
G_LOCK_DEFINE (applications);

static void
close_notification (const char *handle)
{
  g_dbus_connection_call (g_dbus_proxy_get_connection (G_DBUS_PROXY (background_impl)),
                          g_dbus_proxy_get_name (G_DBUS_PROXY (background_impl)),
                          handle,
                          "org.freedesktop.impl.portal.Request",
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static void
remove_outdated_instances (int stamp)
{
  GHashTableIter iter;
  char *id;
  InstanceData *data;
  g_autoptr(GPtrArray) handles = NULL;
  int i;

  handles = g_ptr_array_new_with_free_func (g_free);

  G_LOCK (applications);
  g_hash_table_iter_init (&iter, applications);
  while (g_hash_table_iter_next (&iter, (gpointer *)&id, (gpointer *)&data))
    {
      if (data->stamp < stamp)
        {
          if (data->handle)
            g_ptr_array_add (handles, g_strdup (data->handle));
          g_hash_table_iter_remove (&iter);
        }
    }
  G_UNLOCK (applications);

  for (i = 0; i < handles->len; i++)
    {
      char *handle = g_ptr_array_index (handles, i);
      close_notification (handle);
    }
}

static void
update_background_monitor_properties (void)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aa{sv}"));
  GHashTableIter iter;
  InstanceData *data;
  char *id;

  G_LOCK (applications);
  g_hash_table_iter_init (&iter, applications);
  while (g_hash_table_iter_next (&iter, (gpointer *)&id, (gpointer *)&data))
    {
      g_auto(GVariantBuilder) app_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      const char *app_id;
      const char *id;

      if (data->state != BACKGROUND)
        continue;

      if (!flatpak_instance_is_running (data->instance))
        continue;

      id = flatpak_instance_get_id (data->instance);
      app_id = flatpak_instance_get_app (data->instance);
      g_assert (app_id != NULL);

      g_variant_builder_add (&app_builder, "{sv}", "app_id", g_variant_new_string (app_id));
      g_variant_builder_add (&app_builder, "{sv}", "instance", g_variant_new_string (id));
      if (data->status_message)
        g_variant_builder_add (&app_builder, "{sv}", "message", g_variant_new_string (data->status_message));

      g_variant_builder_add_value (&builder, g_variant_builder_end (&app_builder));
    }
  G_UNLOCK (applications);

  xdp_dbus_background_monitor_set_background_apps (XDP_DBUS_BACKGROUND_MONITOR (background->monitor),
                                                   g_variant_builder_end (&builder));
}

static char *
flatpak_instance_get_display_name (FlatpakInstance *instance)
{
  const char *app_id = flatpak_instance_get_app (instance);
  if (app_id[0] != 0)
    {
      g_autofree char *desktop_id = NULL;
      g_autoptr(GAppInfo) info = NULL;

      desktop_id = g_strconcat (app_id, ".desktop", NULL);
      info = (GAppInfo*)g_desktop_app_info_new (desktop_id);

      if (info)
        return g_strdup (g_app_info_get_display_name (info));
    }

  return g_strdup (app_id);
}

typedef struct {
  char *handle;
  char *app_id;
  char *id;
  char *name;
  XdpPermission perm;
  pid_t child_pid;
} NotificationData;

static void
notification_data_free (gpointer data)
{
  NotificationData *nd = data;

  g_free (nd->handle);
  g_free (nd->app_id);
  g_free (nd->id);
  g_free (nd->name);
  g_free (nd);
}

static void
notify_background_done (GObject *source,
                        GAsyncResult *res,
                        gpointer data)
{
  NotificationData *nd = (NotificationData *)data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) results = NULL;
  guint response;
  guint result;
  InstanceData *idata;

  if (!xdp_dbus_impl_background_call_notify_background_finish (background_impl,
                                                               &response,
                                                               &results,
                                                               res,
                                                               &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error from background backend: %s", error->message);
      notification_data_free (nd);
      return;
    }

  if (!g_variant_lookup (results, "result", "u", &result))
    result = IGNORE;

  if (result == ALLOW)
    {
      g_debug ("Allowing app %s to run in background", nd->app_id);

      if (nd->perm != XDP_PERMISSION_ASK)
        nd->perm = XDP_PERMISSION_YES;
    }
  else if (result == FORBID)
    {
      g_debug ("Forbid app %s to run in background", nd->app_id);

      if (nd->perm != XDP_PERMISSION_ASK)
        nd->perm = XDP_PERMISSION_NO;

      g_message ("Terminating app %s (process %d) because the app does not "
                 "have permission to run in the background. You may be able to "
                 "grant this app the permission to run in background in the "
                 "system settings of your desktop environment.",
                 nd->app_id, nd->child_pid);

      kill (nd->child_pid, SIGKILL);
    }
  else if (result == IGNORE)
    {
      g_debug ("Allow this instance of %s to run in background without permission changes", nd->app_id);
    }
  else
    g_debug ("Unexpected response from NotifyBackground: %u", result);

  if (nd->perm != XDP_PERMISSION_UNSET)
    set_permission (nd->app_id, nd->perm);

  G_LOCK (applications);
  idata = g_hash_table_lookup (applications, nd->id);
  if (idata)
    {
      g_clear_pointer (&idata->handle, g_free);
      idata->permission = nd->perm;
    }
  G_UNLOCK (applications);

  notification_data_free (nd);
}

static void
check_background_apps (void)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GHashTable) app_states = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  int i;
  static int stamp;
  g_autoptr(GPtrArray) notifications = NULL;

  app_states = get_app_states ();
  if (app_states == NULL)
    return;

  g_debug ("Checking background permissions");

  perms = get_all_permissions ();
  instances = flatpak_instance_get_all ();
  notifications = g_ptr_array_new ();

  stamp++;

  G_LOCK (applications);
  for (i = 0; i < instances->len; i++)
    {
      FlatpakInstance *instance = g_ptr_array_index (instances, i);
      const char *id;
      const char *app_id;
      pid_t child_pid;
      InstanceData *idata;
      const char *state_names[] = { "background", "running", "active" };
      gboolean is_new = FALSE;

      if (!flatpak_instance_is_running (instance))
        continue;

      id = flatpak_instance_get_id (instance);
      app_id = flatpak_instance_get_app (instance);
      child_pid = flatpak_instance_get_child_pid (instance);

      idata = g_hash_table_lookup (applications, id);

      if (!app_id)
        continue;

      if (!idata)
        {
          is_new = TRUE;
          idata = g_new0 (InstanceData, 1);
          idata->instance = g_object_ref (instance);
          g_hash_table_insert (applications, g_strdup (id), idata);
        }

      idata->stamp = stamp;
      idata->state = get_one_app_state (app_id, app_states);

      g_debug ("App %s is %s", app_id, state_names[idata->state]);

      idata->permission = get_one_permission (app_id, perms);

      /* If the app is not in the list yet, add it,
       * but don't notify yet - this gives apps some
       * leeway to get their window up. If it is still
       * in the background next time around, we'll proceed
       * to the next step.
       */
      if (idata->state != BACKGROUND || idata->notified || is_new)
        {
          if (idata->notified)
            g_debug ("Already notified app %s ...skipping\n", app_id);
          if (is_new)
            g_debug ("App %s is new ...skipping\n", app_id);
          continue;
        }

      switch (idata->permission)
        {
        case XDP_PERMISSION_NO:
          idata->stamp = 0;

          g_debug ("Kill app %s (pid %d)", app_id, child_pid);
          kill (child_pid, SIGKILL);
          break;

        case XDP_PERMISSION_ASK:
        case XDP_PERMISSION_UNSET:
          {
            NotificationData *nd = g_new0 (NotificationData, 1);

            if (idata->handle)
              {
                close_notification (idata->handle);
                g_free (idata->handle);
              }

            idata->handle = g_strdup_printf ("/org/freedesktop/portal/desktop/notify/background%d", stamp);
            idata->notified = TRUE;

            nd->handle = g_strdup (idata->handle);
            nd->name = flatpak_instance_get_display_name (instance);
            nd->app_id = g_strdup (app_id);
            nd->id = g_strdup (id);
            nd->child_pid = child_pid;
            nd->perm = idata->permission;

            g_ptr_array_add (notifications, nd);
          }
          break;

        case XDP_PERMISSION_YES:
        default:
          break;
        }
    }
  G_UNLOCK (applications);

  for (i = 0; i < notifications->len; i++)
    {
      NotificationData *nd = g_ptr_array_index (notifications, i);

      g_debug ("Notify background for %s", nd->app_id);

      xdp_dbus_impl_background_call_notify_background (background_impl,
                                                       nd->handle,
                                                       nd->app_id,
                                                       nd->name,
                                                       NULL,
                                                       notify_background_done,
                                                       nd);
    }

  remove_outdated_instances (stamp);
  update_background_monitor_properties ();
}

static GMainContext *monitor_context;

static gpointer
background_monitor (gpointer data)
{
  applications = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, instance_data_free);

  while (TRUE)
    {
      g_main_context_iteration (monitor_context, TRUE);
      /* We check twice, to avoid killing unlucky apps hit at a bad time */
      sleep (5);
      check_background_apps ();
      sleep (5);
      check_background_apps ();
    }

  g_clear_pointer (&applications, g_hash_table_unref);
  g_clear_pointer (&monitor_context, g_main_context_unref);

  return NULL;
}

static void
start_background_monitor (void)
{
  g_autoptr(GThread) thread = NULL;

  g_debug ("Starting background app monitor");

  monitor_context = g_main_context_new ();

  thread = g_thread_new ("background monitor", background_monitor, NULL);
}

static void
running_apps_changed (gpointer data)
{
  g_debug ("Running app windows changed, wake up monitor thread");
  g_main_context_wakeup (monitor_context);
}

static void
instances_changed (gpointer data)
{
  g_debug ("Running instances changed, wake up monitor thread");
  g_main_context_wakeup (monitor_context);
}

gboolean
enable_autostart_sync (XdpAppInfo          *app_info,
                       gboolean             enable,
                       const char * const  *autostart_exec,
                       gboolean             activatable,
                       gboolean            *out_enabled,
                       GError             **error)
{
  GStrv exec = (GStrv)autostart_exec;
  g_autofree char *cmd = NULL;
  g_autofree char *file = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  const char *appid = xdp_app_info_get_id (app_info);
  GAppInfo *info = xdp_app_info_get_gappinfo (app_info);

  if (g_strcmp0 (appid, "") == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Autostart not supported (no AppId detected)");
      return FALSE;
    }

  file = g_strconcat (appid, ".desktop", NULL);
  dir = g_build_filename (g_get_user_config_dir (), "autostart", NULL);
  path = g_build_filename (dir, file, NULL);

  if (!enable)
    {
      unlink (path);

      *out_enabled = FALSE;
      return TRUE;
    }

  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create directory for Autostart files");
      return FALSE;
    }

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_TYPE,
                         "Application");
  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_NAME,
                         info ? g_app_info_get_name (info) : appid);
  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         "X-XDP-Autostart",
                         appid);

  if (exec)
    cmd = g_strjoinv (" ", exec);

  if (cmd)
    {
      g_key_file_set_string (keyfile,
                             G_KEY_FILE_DESKTOP_GROUP,
                             G_KEY_FILE_DESKTOP_KEY_EXEC,
                             cmd);
    }

  if (activatable)
    {
      g_key_file_set_boolean (keyfile,
                              G_KEY_FILE_DESKTOP_GROUP,
                              G_KEY_FILE_DESKTOP_KEY_DBUS_ACTIVATABLE,
                              TRUE);
    }

  if (!xdp_app_info_validate_autostart (app_info, keyfile, autostart_exec,
                                        NULL, error))
    return FALSE;

  if (!g_key_file_save_to_file (keyfile, path, error))
    return FALSE;

  *out_enabled = TRUE;
  return TRUE;
}

static void
handle_request_background_in_thread_func (GTask *task,
                                          gpointer source_object,
                                          gpointer task_data,
                                          GCancellable *cancellable)
{
  XdpRequest *request = XDP_REQUEST (task_data);
  GVariant *options;
  const char *id;
  XdpPermission permission;
  const char *reason = NULL;
  gboolean autostart_requested = FALSE;
  gboolean autostart_enabled;
  gboolean allowed;
  g_autoptr(GError) error = NULL;
  g_autofree const char **autostart_exec = { NULL };
  gboolean activatable = FALSE;

  REQUEST_AUTOLOCK (request);

  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");
  g_variant_lookup (options, "reason", "&s", &reason);
  g_variant_lookup (options, "autostart", "b", &autostart_requested);
  g_variant_lookup (options, "commandline", "^a&s", &autostart_exec);
  g_variant_lookup (options, "dbus-activatable", "b", &activatable);

  id = xdp_app_info_get_id (request->app_info);

  if (xdp_app_info_is_host (request->app_info))
    permission = XDP_PERMISSION_YES;
  else
    permission = get_permission (id);

  g_debug ("Handle RequestBackground for '%s'", id);

  if (permission == XDP_PERMISSION_ASK)
    {
      g_auto(GVariantBuilder) opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autofree char *app_id = NULL;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      GAppInfo *info = NULL;

      info = xdp_app_info_get_gappinfo (request->app_info);
      app_id = info ? xdp_get_app_id_from_desktop_id (g_app_info_get_id (info)) : g_strdup (id);

      title = g_strdup_printf (_("Allow %s to Run in the Background?"), info ? g_app_info_get_display_name (info) : id);
      if (reason)
        subtitle = g_strdup (reason);
      else if (autostart_requested)
        subtitle = g_strdup_printf (_("%s wants to be started automatically and run in the background"), info ? g_app_info_get_display_name (info) : id);
      else
        subtitle = g_strdup_printf (_("%s wants to run in the background"), info ? g_app_info_get_display_name (info) : id);
      body = g_strdup (_("The ‘run in background’ permission can be changed at any time from the app settings"));

      g_debug ("Calling backend for background access for: %s", id);

      g_variant_builder_add (&opt_builder, "{sv}", "deny_label", g_variant_new_string (_("Don't allow")));
      g_variant_builder_add (&opt_builder, "{sv}", "grant_label", g_variant_new_string (_("Allow")));
      if (!xdp_dbus_impl_access_call_access_dialog_sync (access_impl,
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
    }
  else
    {
      allowed = permission != XDP_PERMISSION_NO;
      if (permission == XDP_PERMISSION_UNSET)
        set_permission (id, XDP_PERMISSION_YES);
    }

  g_debug ("Setting autostart for %s to %s", id,
           allowed && autostart_requested ? "enabled" : "disabled");

  autostart_enabled = FALSE;

  if (!enable_autostart_sync (request->app_info,
                              allowed && autostart_requested,
                              autostart_exec,
                              activatable,
                              &autostart_enabled,
                              &error))
    {
      g_warning ("EnableAutostart call failed: %s", error->message);
      g_clear_error (&error);
    }

  if (request->exported)
    {
      XdgDesktopPortalResponseEnum portal_response;
      g_auto(GVariantBuilder) results =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      g_variant_builder_add (&results, "{sv}", "background", g_variant_new_boolean (allowed));
      g_variant_builder_add (&results, "{sv}", "autostart", g_variant_new_boolean (autostart_enabled));

      if (allowed)
        portal_response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
      else
        portal_response =  XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      portal_response,
                                      g_variant_builder_end (&results));
      xdp_request_unexport (request);
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

static gboolean
validate_commandline (const char *key,
                      GVariant *value,
                      GVariant *options,
                      GError **error)
{
  gsize length;
  g_autofree const char **strv = g_variant_get_strv (value, &length);

  if (strv[0] == NULL)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Commandline can't be empty");
      return FALSE;
    }

  if (g_utf8_strlen (strv[0], -1) > 256)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting overly long commandlines");
      return FALSE;
    }

  if (*strv[0] == ' ' || *strv[0] == '-')
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "First commandline item can't start with whitespace nor hyphens");
      return FALSE;
    }

  if (length > 100)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting overly long commandlines");
      return FALSE;
    }

  return TRUE;
}
static XdpOptionKey background_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason },
  { "autostart", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "commandline", G_VARIANT_TYPE_STRING_ARRAY, validate_commandline },
  { "dbus-activatable", G_VARIANT_TYPE_BOOLEAN, NULL },
};

static gboolean
handle_request_background (XdpDbusBackground *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_window,
                           GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  if (!xdp_filter_options (arg_options, &opt_builder,
                           background_options, G_N_ELEMENTS (background_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (access_impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (access_impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_background_complete_request_background (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_request_background_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
set_status_finished_cb (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  GDBusMethodInvocation *invocation;
  g_autoptr(GError) error = NULL;

  g_assert (g_task_is_valid (result, source_object));

  invocation = g_task_get_task_data (G_TASK (result));
  g_assert (invocation != NULL);

  if (g_task_propagate_boolean (G_TASK (result), &error))
    {
      xdp_dbus_background_complete_set_status (XDP_DBUS_BACKGROUND (source_object),
                                               invocation);
    }
  else
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
    }
}

static void
handle_set_status_in_thread_func (GTask        *task,
                                  gpointer      source_object,
                                  gpointer      task_data,
                                  GCancellable *cancellable)
{
  GDBusMethodInvocation *invocation = task_data;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autofree char *message = NULL;
  InstanceData *data;
  const char *id = NULL;
  GVariant *options;

  id = xdp_app_info_get_instance (app_info);

  options = g_object_get_data (G_OBJECT (invocation), "options");
  g_variant_lookup (options, "message", "s", &message);

  G_LOCK (applications);
  data = g_hash_table_lookup (applications, id);

  if (!data)
    {
      g_autoptr(GHashTable) app_states = NULL;
      g_autoptr(GPtrArray) instances = NULL;
      FlatpakInstance *instance = NULL;

      instances = flatpak_instance_get_all ();
      for (guint i = 0; i < instances->len; i++)
        {
          FlatpakInstance *aux = g_ptr_array_index (instances, i);
          if (g_strcmp0 (id, flatpak_instance_get_id (aux)) == 0)
            {
              instance = aux;
              break;
            }
        }

      if (!instance)
        {
          G_UNLOCK (applications);
          g_task_return_new_error (task,
                                   XDG_DESKTOP_PORTAL_ERROR,
                                   XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                   "No sandboxed instance of the application found");
          return;
        }

      app_states = get_app_states ();
      if (app_states == NULL)
        {
          G_UNLOCK (applications);
          g_task_return_new_error (task,
                                   XDG_DESKTOP_PORTAL_ERROR,
                                   XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                   "Could not fetch app state from backend");
          return;
        }

      data = g_new0 (InstanceData, 1);
      data->instance = g_object_ref (instance);
      data->state = get_one_app_state (xdp_app_info_get_id (app_info), app_states);
      g_hash_table_insert (applications, g_strdup (id), data);
    }

  g_assert (data != NULL);
  g_clear_pointer (&data->status_message, g_free);
  data->status_message = g_steal_pointer (&message);

  G_UNLOCK (applications);

  update_background_monitor_properties ();

  g_task_return_boolean (task, TRUE);
}

static gboolean
validate_message (const char  *key,
                  GVariant    *value,
                  GVariant    *options,
                  GError     **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (g_utf8_strlen (string, -1) > 96)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Status message is longer than 96 characters");
      return FALSE;
    }

  if (strstr (string, "\n"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Status message must not have newlines");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey set_status_options[] = {
  { "message", G_VARIANT_TYPE_STRING, validate_message },
};

static gboolean
handle_set_status (XdpDbusBackground     *object,
                   GDBusMethodInvocation *invocation,
                   GVariant              *arg_options)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *id = NULL;

  g_debug ("Handling SetStatus call from %s", xdp_app_info_get_id (app_info));

  if (xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Only sandboxed applications can set background status");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  id = xdp_app_info_get_instance (app_info);
  if (!id)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "No sandboxed instance of the application found");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &opt_builder,
                           set_status_options,
                           G_N_ELEMENTS (set_status_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

  g_object_set_data_full (G_OBJECT (invocation),
                          "options",
                          g_steal_pointer (&options),
                          (GDestroyNotify) g_variant_unref);

  task = g_task_new (object, NULL, set_status_finished_cb, NULL);
  g_task_set_task_data (task, g_object_ref (invocation), g_object_unref);
  g_task_run_in_thread (task, handle_set_status_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
background_iface_init (XdpDbusBackgroundIface *iface)
{
  iface->handle_request_background = handle_request_background;
  iface->handle_set_status = handle_set_status;
}

static void
background_init (Background *background)
{
  xdp_dbus_background_set_version (XDP_DBUS_BACKGROUND (background), 2);
}

static void
background_class_init (BackgroundClass *klass)
{
}

void
init_background (XdpContext *context)
{
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *access_impl_config;
  XdpImplConfig *impl_config;
  g_autofree char *instance_path = NULL;
  g_autoptr(GFile) instance_dir = NULL;
  g_autoptr(GError) error = NULL;

  access_impl_config =
    xdp_portal_config_find (config, "org.freedesktop.impl.portal.Access");

  impl_config = xdp_portal_config_find (config,
                                        "org.freedesktop.impl.portal.Background");

  if (access_impl_config == NULL || impl_config == NULL)
    return;

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     access_impl_config->dbus_name,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL, &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  background_impl = xdp_dbus_impl_background_proxy_new_sync (connection,
                                                             G_DBUS_PROXY_FLAGS_NONE,
                                                             impl_config->dbus_name,
                                                             DESKTOP_PORTAL_OBJECT_PATH,
                                                             NULL,
                                                             &error);
  if (background_impl == NULL)
    {
      g_warning ("Failed to create background proxy: %s", error->message);
      return;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (background_impl), G_MAXINT);

  background = g_object_new (background_get_type (), NULL);

  background->monitor = xdp_background_monitor_new (NULL, &error);
  if (background->monitor == NULL)
    {
      g_warning ("Failed to create background monitor: %s", error->message);
      return;
    }

  start_background_monitor ();

  g_signal_connect (background_impl, "running-applications-changed",
                    G_CALLBACK (running_apps_changed), NULL);

  /* FIXME: it would be better if libflatpak had a monitor api for this */
  instance_path = g_build_filename (g_get_user_runtime_dir (), ".flatpak", NULL);
  instance_dir = g_file_new_for_path (instance_path);
  instance_monitor = g_file_monitor_directory (instance_dir, G_FILE_MONITOR_NONE, NULL, &error);
  if (!instance_monitor)
    g_warning ("Failed to create a monitor for %s: %s", instance_path, error->message);
  else
    g_signal_connect (instance_monitor, "changed", G_CALLBACK (instances_changed), NULL);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (background),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
