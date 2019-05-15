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

#include <flatpak.h>
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
static XdpImplBackground *background_impl;
static Background *background;

GType background_get_type (void) G_GNUC_CONST;
static void background_iface_init (XdpBackgroundIface *iface);

G_DEFINE_TYPE_WITH_CODE (Background, background, XDP_TYPE_BACKGROUND_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_BACKGROUND, background_iface_init));


typedef enum { UNSET, NO, YES, ASK } Permission;

static GVariant *
get_all_permissions (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

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
      return NULL;
    }

  return g_steal_pointer (&out_perms);
}

static Permission
get_one_permission (const char *app_id,
                    GVariant   *perms)
{
  const char **permissions;

  if (perms == NULL)
    {
      g_debug ("No background permissions found");

      return UNSET;
    }
  else if (!g_variant_lookup (perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No background permissions stored for: app %s", app_id);

      return UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong background permission format, ignoring (%s)", a);
      return UNSET;
    }

  g_debug ("permission store: background, app %s -> %s", app_id, permissions[0]);

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

static Permission
get_permission (const char *app_id)
{
  g_autoptr(GVariant) perms = NULL;

  perms = get_all_permissions ();
  if (perms)
    return get_one_permission (app_id, perms);

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

/* This is conservative, but lets us avoid escaping most
   regular Exec= lines, which is nice as that can sometimes
   cause problems for apps launching desktop files. */
static gboolean
need_quotes (const char *str)
{
  const char *p;

  for (p = str; *p; p++)
    {
      if (!g_ascii_isalnum (*p) &&
          strchr ("-_%.=:/@", *p) == NULL)
        return TRUE;
    }

  return FALSE;
}

static char *
maybe_quote (const char *str)
{
  if (need_quotes (str))
    return g_shell_quote (str);
  return g_strdup (str);
}

static char **
rewrite_commandline (const char *app_id,
                     const char * const *commandline)
{
  g_autoptr(GPtrArray) args = NULL;

  args = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("run"));
  if (commandline && commandline[0])
    {
      int i;
      g_autofree char *cmd = NULL;

      cmd = maybe_quote (commandline[0]);
      g_ptr_array_add (args, g_strdup_printf ("--command=%s", cmd));
      g_ptr_array_add (args, g_strdup (app_id));
      for (i = 1; commandline[i]; i++)
        g_ptr_array_add (args, g_strdup (commandline[i]));
    }
  else
    g_ptr_array_add (args, g_strdup (app_id));
  g_ptr_array_add (args, NULL);

  return (char **)g_ptr_array_free (g_steal_pointer (&args), FALSE);
}

typedef enum {
  AUTOSTART_FLAGS_NONE        = 0,
  AUTOSTART_FLAGS_ACTIVATABLE = 1 << 0,
} AutostartFlags;

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
  const char *reason = NULL;
  gboolean autostart_requested = FALSE;
  gboolean autostart_enabled;
  gboolean allowed;
  g_autoptr(GError) error = NULL;
  const char * const *autostart_exec = { NULL };
  AutostartFlags autostart_flags = AUTOSTART_FLAGS_NONE;
  gboolean activatable = FALSE;
  g_auto(GStrv) commandline = NULL;

  REQUEST_AUTOLOCK (request);

  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");
  g_variant_lookup (options, "reason", "&s", &reason);
  g_variant_lookup (options, "autostart", "b", &autostart_requested);
  g_variant_lookup (options, "commandline", "^a&s", &autostart_exec);
  g_variant_lookup (options, "dbus-activatable", "b", &activatable);

  if (activatable)
    autostart_flags |= AUTOSTART_FLAGS_ACTIVATABLE;

  app_id = xdp_app_info_get_id (request->app_info);

  if (xdp_app_info_is_host (request->app_info))
    permission = YES;
  else
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

      info = xdp_app_info_load_app_info (request->app_info);

      title = g_strdup_printf (_("Allow %s to run in the background?"), info ? g_app_info_get_display_name (info) : app_id);
      if (reason)
        subtitle = g_strdup (reason);
      else if (autostart_requested)
        subtitle = g_strdup_printf (_("%s requests to be started automatically and run in the background."), info ? g_app_info_get_display_name (info) : app_id);
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

  g_debug ("Setting autostart for %s to %s", app_id,
           allowed && autostart_requested ? "enabled" : "disabled");

  commandline = rewrite_commandline (app_id, autostart_exec);
  if (!xdp_impl_background_call_enable_autostart_sync (background_impl,
                                                       app_id,
                                                       allowed && autostart_requested,
                                                       (const char * const *)commandline,
                                                       autostart_flags,
                                                       &autostart_enabled,
                                                       NULL,
                                                       &error))
    {
      g_warning ("EnableAutostart call failed: %s", error->message);
      g_clear_error (&error);
    }

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&results, "{sv}", "background", g_variant_new_boolean (allowed));
      g_variant_builder_add (&results, "{sv}", "autostart", g_variant_new_boolean (autostart_enabled));
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
  { "reason", G_VARIANT_TYPE_STRING, validate_reason },
  { "autostart", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "commandline", G_VARIANT_TYPE_STRING_ARRAY, NULL },
  { "dbus-activatable", G_VARIANT_TYPE_BOOLEAN, NULL },
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

/* background monitor */

typedef enum { BACKGROUND, RUNNING, ACTIVE } AppState;

static GHashTable *
get_app_states (void)
{
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GHashTable) app_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *appid;
  GVariant *value;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_background_call_get_app_state_sync (background_impl, &apps, NULL, &error))
    {
      g_warning ("Failed to get application states: %s", error->message);
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

static GHashTable *applications;
G_LOCK_DEFINE_STATIC (applications);

static void
close_notification (const char *handle)
{
  g_dbus_connection_call (g_dbus_proxy_get_connection (G_DBUS_PROXY (background)),
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
remove_outdated_applications (GPtrArray *apps)
{
  int j;
  GHashTableIter iter;
  char *app_id;
  char *handle;
  g_autoptr(GPtrArray) handles = NULL;

  handles = g_ptr_array_new_with_free_func (g_free);

  G_LOCK (applications);
  g_hash_table_iter_init (&iter, applications);
  while (g_hash_table_iter_next (&iter, (gpointer *)&app_id, (gpointer *)&handle))
    {
      gboolean found = FALSE;
      for (j = 0; j < apps->len && !found; j++)
        {
          FlatpakInstance *app = g_ptr_array_index (apps, j);
          found = g_strcmp0 (app_id, flatpak_instance_get_app (app));
        }
      if (!found)
        {
          g_hash_table_iter_remove (&iter);
          if (handle)
            g_ptr_array_add (handles, g_strdup (handle));
        }
    }
  G_UNLOCK (applications);

  for (j = 0; j < handles->len; j++)
    { 
      const char *handle = g_ptr_array_index (handles, j);
      close_notification (handle);
    }
}

/*
 * Returns whether the @app_id was found in the
 * table of background apps, and if so, sets
 * @value to the value found for it.
 */
static gboolean
lookup_background_app (const char *app_id,
                       char **value)
{
  gboolean res;
  char *orig_key;
  char *orig_val;

  G_LOCK (applications);
  res = g_hash_table_lookup_extended (applications, app_id, (gpointer *)&orig_key, (gpointer *)&orig_val);
  if (res) 
    *value = g_strdup (orig_val);
  G_UNLOCK (applications);

  return res;
}

static void
add_background_app (const char *app_id,
                    const char *handle)
{
  G_LOCK (applications);
  g_hash_table_insert (applications, g_strdup (app_id), g_strdup (handle));
  G_UNLOCK (applications);
}

static void
remove_background_app (const char *app_id)
{
  g_autofree char *handle = NULL;

  G_LOCK (applications);
  handle = g_strdup (g_hash_table_lookup (applications, app_id));
  g_hash_table_remove (applications, app_id);
  G_UNLOCK (applications);

  if (handle)
    close_notification (handle);
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

static FlatpakInstance *
find_instance (const char *app_id)
{
  g_autoptr(GPtrArray) instances = NULL;
  int i;

  instances = flatpak_instance_get_all ();
  for (i = 0; i < instances->len; i++)
    {
      FlatpakInstance *inst = g_ptr_array_index (instances, i);

      if (g_str_equal (flatpak_instance_get_app (inst), app_id))
        return g_object_ref (inst);
    }

  return NULL;
}

static void
kill_app (const char *app_id)
{
  g_autoptr(FlatpakInstance) instance = NULL;

  g_debug ("Killing app %s", app_id);

  instance = find_instance (app_id);

  if (instance)
    kill (flatpak_instance_get_child_pid (instance), SIGKILL);
}

typedef struct {
  char *app_id;
  Permission perm;
} DoneData;

static void
done_data_free (gpointer data)
{
  DoneData *ddata = data;

  g_free (ddata->app_id);
  g_free (ddata);
}

static void
notify_background_done (GObject *source,
                        GAsyncResult *res,
                        gpointer data)
{
  DoneData *ddata = (DoneData *)data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) results = NULL;
  guint response;
  guint result;

  if (!xdp_impl_background_call_notify_background_finish (background_impl,
                                                          &response,
                                                          &results,
                                                          res,
                                                          &error))
    {
      g_warning ("Error from background backend: %s", error->message);
      done_data_free (ddata);
      return;
    }

  g_variant_lookup (results, "result", "u", &result);

  if (result == 1)
    {
      g_debug ("Allowing app %s to run in background", ddata->app_id);
      if (ddata->perm != ASK)
        set_permission (ddata->app_id, YES);
    }
  else if (result == 0)
    {
      g_debug ("Forbid app %s to run in background", ddata->app_id);

      if (ddata->perm != ASK)
        set_permission (ddata->app_id, NO);
      kill_app (ddata->app_id);
    }
  else
    g_debug ("Unexpected response from NotifyBackground: %u", result);

  add_background_app (ddata->app_id, NULL);
  done_data_free (ddata);
}

static void
send_notification (FlatpakInstance *instance,
                   Permission       permission)
{
  DoneData *ddata;
  g_autofree char *name = flatpak_instance_get_display_name (instance);
  g_autofree char *handle = NULL;
  static int count;

  ddata = g_new (DoneData, 1);
  ddata->app_id = g_strdup (flatpak_instance_get_app (instance));
  ddata->perm = permission;

  g_debug ("Notify background for %s", ddata->app_id);

  handle = g_strdup_printf ("/org/freedesktop/portal/desktop/notify/background%d", count++);

  add_background_app (ddata->app_id, handle);

  xdp_impl_background_call_notify_background (background_impl,
                                              handle,
                                              ddata->app_id,
                                              name,
                                              NULL,
                                              notify_background_done,
                                              ddata);
}

static void
check_background_apps (void)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GHashTable) app_states = NULL;
  g_autoptr(GPtrArray) apps = NULL;
  int i;

  app_states = get_app_states ();
  if (app_states == NULL)
    return;

  g_debug ("Checking background permissions");

  perms = get_all_permissions ();
  apps = flatpak_instance_get_all ();

  remove_outdated_applications (apps);

  for (i = 0; i < apps->len; i++)
    {
      FlatpakInstance *instance = g_ptr_array_index (apps, i);
      const char *app_id = flatpak_instance_get_app (instance);
      Permission permission;
      AppState state;
      const char *state_names[] = { "background", "running", "active" };
      g_autofree char *handle = NULL;

      if (!flatpak_instance_is_running (instance))
        continue;

      state = get_one_app_state (app_id, app_states);
      g_debug ("App %s is %s", app_id, state_names[state]);

      if (state != BACKGROUND)
        {
          remove_background_app (app_id);
          continue;
        }

      /* If the app is not in the list of background apps
       * yet, add it, but don't notify yet - this gives
       * apps some leeway to get their window app. If it
       * is still in the background next time around,
       * we'll proceed to the next step.
       */
      if (!lookup_background_app (app_id, &handle))
        {
          add_background_app (app_id, NULL);
          continue;
        }

      if (handle)
        continue; /* already notified */

      permission = get_one_permission (app_id, perms);
      if (permission == NO)
        {
          pid_t pid = flatpak_instance_get_child_pid (instance);
          g_debug ("Killing app %s (child pid %u)", app_id, pid);
          kill (pid, SIGKILL);
        }
      else if (permission == ASK || permission == UNSET)
        {
          send_notification (instance, permission);
        }
    }
}

static gpointer
background_monitor (gpointer data)
{
  while (1)
    {
      check_background_apps ();
      sleep (60);
    }

  return NULL;
}

static void
start_background_monitor (void)
{
  g_autoptr(GThread) thread = NULL;

  applications = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_debug ("Starting background app monitor");

  thread = g_thread_new ("background monitor", background_monitor, NULL);
}

GDBusInterfaceSkeleton *
background_create (GDBusConnection *connection,
                   const char *dbus_name_access,
                   const char *dbus_name_background)
{
  g_autoptr(GError) error = NULL;

  access_impl = xdp_impl_access_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                dbus_name_access,
                                                DESKTOP_PORTAL_OBJECT_PATH,
                                                NULL,
                                                &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  background_impl = xdp_impl_background_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        dbus_name_background,
                                                        DESKTOP_PORTAL_OBJECT_PATH,
                                                        NULL,
                                                        &error);
  if (background_impl == NULL)
    {
      g_warning ("Failed to create background proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (background_impl), G_MAXINT);
  background = g_object_new (background_get_type (), NULL);

  start_background_monitor ();

  return G_DBUS_INTERFACE_SKELETON (background);
}
