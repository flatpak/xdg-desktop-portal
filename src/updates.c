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
#include "updates.h"
#include "request.h"
#include "session.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "updates"
#define PERMISSION_ID "updates"

typedef struct _Updates Updates;
typedef struct _UpdatesClass UpdatesClass;

struct _Updates
{
  XdpUpdatesSkeleton parent_instance;
};

struct _UpdatesClass
{
  XdpUpdatesSkeletonClass parent_class;
};

static XdpImplAccess *impl;
static Updates *updates;

GType updates_get_type (void) G_GNUC_CONST;
static void updates_iface_init (XdpUpdatesIface *iface);

G_DEFINE_TYPE_WITH_CODE (Updates, updates, XDP_TYPE_UPDATES_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_UPDATES, updates_iface_init));

typedef enum { UNSET, ASK, YES, NO } Permission;

static guint32
get_permission (const char *app_id)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;
  guint32 ret;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSION_TABLE,
                                                   PERMISSION_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No updates permissions found: %s", error->message);
      g_clear_error (&error);
    }

  if (out_perms != NULL)
    {
      const char **perms;
      if (g_variant_lookup (out_perms, app_id, "^a&s", &perms))
        {
          if (strcmp (perms[0], "ask") == 0)
            return ASK;
          else if (strcmp (perms[0], "yes") == 0)
            return YES;
          else
            return NO;
        }
    }
  else
    ret = UNSET;

  g_debug ("Updates permissions for %s: %d", app_id, ret);

  return ret;
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


typedef struct {
  Request *request;
  int n_ops;
  int op;
  int progress;
} UpdatesData;

static void
updates_data_free (gpointer data)
{
  UpdatesData *d = data;

  g_clear_object (&d->request);
  g_free (d);
}

static void
emit_progress (UpdatesData *d)
{
  GDBusConnection *connection;
  GVariantBuilder builder;
  g_autoptr(GError) error = NULL;

  g_debug ("%d/%d ops, progress %d\n", d->op, d->n_ops, d->progress);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "op", g_variant_new_int32 (d->op));
  g_variant_builder_add (&builder, "{sv}", "n_ops", g_variant_new_int32 (d->n_ops));
  g_variant_builder_add (&builder, "{sv}", "progress", g_variant_new_int32 (d->progress));

  connection = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (d->request));
  if (!g_dbus_connection_emit_signal (connection,
                                      d->request->sender,
                                      "/org/freedesktop/portal/desktop",
                                      "org.freedesktop.portal.Updates",
                                      "Progress",
                                      g_variant_new ("(oa{sv})", d->request->id, &builder),
                                      &error))
    {
      g_warning ("Failed to emit ::progress: %s", error->message);
    }
}

static gboolean
ready (FlatpakTransaction *transaction,
       gpointer data)
{
  UpdatesData *d = data;

  d->n_ops = g_list_length (flatpak_transaction_get_operations (transaction));
  d->op = 0;

  emit_progress (d);

  return TRUE;
}

static void
progress_changed (FlatpakTransactionProgress *progress,
                  gpointer data)
{
  UpdatesData *d = data;

  d->progress = flatpak_transaction_progress_get_progress (progress);
  emit_progress (d);
}

static void
new_operation (FlatpakTransaction *transaction,
               FlatpakTransactionOperation *op,
               FlatpakTransactionProgress *progress,
               gpointer data)
{
  UpdatesData *d = data;

  d->progress = 0;
  emit_progress (d);
  g_signal_connect (progress, "changed", G_CALLBACK (progress_changed), data);
}

static void
operation_done (FlatpakTransaction *transaction,
                FlatpakTransactionOperation *op,
                const char *commit,
                FlatpakTransactionResult result,
                gpointer data)
{
  UpdatesData *d = data;

  emit_progress (d);
  d->op++;
}

static FlatpakInstallation *
xdp_app_info_get_installation (XdpAppInfo *app_info)
{
  g_autofree char *path = NULL;
  FlatpakInstallation *installation;

  path = xdp_app_info_get_inst_path (app_info);
  if (strstr (path, ".local") != NULL)
    installation = flatpak_installation_new_user (NULL, NULL);
  else
    {
      g_autoptr(GFile) file = g_file_new_for_path (path);
      installation = flatpak_installation_new_for_path (file, FALSE, NULL, NULL);
    }

  return installation;
}

static void
handle_install_update_in_thread_func (GTask *task,
                                      gpointer source_object,
                                      gpointer task_data,
                                      GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *app_id;
  Permission permission;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakInstallation) installation = NULL;
  FlatpakTransaction *transaction;
  UpdatesData *d;
  g_autofree char *ref = NULL;
  GVariantBuilder opt_builder;
  guint response = 2;

  REQUEST_AUTOLOCK (request);

  d = g_new0 (UpdatesData, 1);
  d->request = g_object_ref (request);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  app_id = xdp_app_info_get_id (request->app_info);
  permission = get_permission (app_id);

  if (permission == UNSET || permission == ASK)
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      g_autoptr(XdpImplRequest) impl_request = NULL;
      GVariantBuilder access_opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      const char *body;
      const char *window;

      window = (const char *)g_object_get_data (G_OBJECT (request), "window");

      impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                      request->id,
                                                      NULL, NULL);

      request_set_impl_request (request, impl_request);

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Update")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("package-x-generic-symbolic"));

      if (g_str_equal (app_id, ""))
        {
          title = g_strdup (_("Update this application?"));
          subtitle = g_strdup (_("An application wants to update itself."));
        }
      else
        {
          g_autofree char *id = NULL;
          g_autoptr(GDesktopAppInfo) info = NULL;
          const char *name;

          id = g_strconcat (app_id, ".desktop", NULL);
          info = g_desktop_app_info_new (id);
          name = g_app_info_get_display_name (G_APP_INFO (info));

          title = g_strdup_printf (_("Update %s?"), name);
          subtitle = g_strdup (_("The application wants to update itself."));
        }

      body = _("Update access can be changed any time from the privacy settings.");

      if (!xdp_impl_access_call_access_dialog_sync (impl,
                                                    request->id,
                                                    app_id,
                                                    window,
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

      if (permission == UNSET)
        set_permission (app_id, (access_response == 0) ? YES : NO);

      permission = (access_response == 0) ? YES : NO;
    }

  if (permission == NO)
    goto out;

  g_debug ("Installing update for %s", app_id);

#if 1
  ref = xdp_app_info_get_ref (request->app_info);
  installation = xdp_app_info_get_installation (request->app_info);
#else
  ref = g_strdup ("app/org.gnome.Documents/x86_64/master");
  installation = flatpak_installation_new_user (NULL, &error);
#endif

  if (installation == NULL)
    {
      g_warning ("Failed to get installation for %s: %s", ref, error->message);
      goto out;
    }

  transaction = flatpak_transaction_new_for_installation (installation, NULL, &error);
  if (transaction == NULL)
    {
      g_warning ("Failed to create transaction: %s", error->message);
      goto out;
    }

  flatpak_transaction_add_default_dependency_sources (transaction);
  if (!flatpak_transaction_add_update (transaction, ref, NULL, NULL, &error))
    goto out;

  g_signal_connect (transaction, "ready", G_CALLBACK (ready), d);
  g_signal_connect (transaction, "new-operation", G_CALLBACK (new_operation), d);
  g_signal_connect (transaction, "operation-done", G_CALLBACK (operation_done), d);

  if (!flatpak_transaction_run (transaction, NULL, &error))
    {
      g_warning ("Transaction failed: %s", error->message);
      goto out;
    }

  response = 0;

out:
  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request), response, g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }

  updates_data_free (d);
}

static gboolean
handle_install_update (XdpUpdates *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_window,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  REQUEST_AUTOLOCK (request);

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_install_update_in_thread_func);

  xdp_updates_complete_install_update (object, invocation, request->id);

  return TRUE;
}

typedef struct _UpdatesSession
{
  Session parent;
  char *ref;
  FlatpakInstallation *installation;
  guint timeout;

  gboolean closed;
} UpdatesSession;

typedef struct _UpdatesSessionClass
{
  SessionClass parent_class;
} UpdatesSessionClass;

GType updates_session_get_type (void);

G_DEFINE_TYPE (UpdatesSession, updates_session, session_get_type ())

static void
updates_session_close (Session *session)
{
  UpdatesSession *updates_session = (UpdatesSession *)session;

  updates_session->closed = TRUE;

  g_debug ("updates session owned by '%s' closed", session->sender);
}

static void
updates_session_finalize (GObject *object)
{
  UpdatesSession *session = (UpdatesSession*)object;

  if (session->timeout)
    g_source_remove (session->timeout);
  g_free (session->ref);
  g_clear_object (&session->installation);

  G_OBJECT_CLASS (updates_session_parent_class)->finalize (object);
}

static void
updates_session_init (UpdatesSession *session)
{
}

static void
updates_session_class_init (UpdatesSessionClass *klass)
{
  GObjectClass *object_class;
  SessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = updates_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = updates_session_close;
}

static gboolean
check_for_updates (gpointer data)
{
  Session *session = data;
  UpdatesSession *usession = data;
  g_autoptr(GPtrArray) updates = NULL;
  int i;

  g_debug ("Checking for updates for %s", usession->ref);

  updates = flatpak_installation_list_installed_refs_for_update (usession->installation, NULL, NULL);

  for (i = 0; i < updates->len; i++)
    {
      FlatpakInstalledRef *iref = g_ptr_array_index (updates, i);
      g_autofree char *ref = flatpak_ref_format_ref (FLATPAK_REF (iref));
      if (g_str_equal (ref, usession->ref))
        {
          GVariantBuilder builder;
          g_autoptr(GError) error = NULL;

          g_debug ("Found update for %s", usession->ref);

          g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

          if (!g_dbus_connection_emit_signal (session->connection,
                                              session->sender,
                                              "/org/freedesktop/portal/desktop",
                                              "org.freedesktop.portal.Updates",
                                              "UpdateAvailable",
                                              g_variant_new ("(oa{sv})", session->id, &builder),
                                              &error))
            {
              g_warning ("Failed to emit UpdateAvailable: %s", error->message);
            }
          break;
        }
    }

  return G_SOURCE_CONTINUE;
}

static void
start_monitoring (UpdatesSession *session)
{
  check_for_updates (session);

  session->timeout = g_timeout_add_seconds (3600, check_for_updates, session);
}

static UpdatesSession *
updates_session_new (GVariant *options,
                     Request *request,
                     GError **error)
{
  Session *session;
  const char *session_token;

  GDBusInterfaceSkeleton *interface_skeleton = G_DBUS_INTERFACE_SKELETON (request);
  GDBusConnection *connection = g_dbus_interface_skeleton_get_connection (interface_skeleton);
  GDBusConnection *impl_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (impl));

  session_token = lookup_session_token (options);
  session = g_initable_new (updates_session_get_type (), NULL, error,
                            "sender", request->sender,
                            "app-id", xdp_app_info_get_id (request->app_info),
                            "token", session_token,
                            "connection", connection,
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);

  if (session)
    {
      UpdatesSession *updates_session = (UpdatesSession*)session;

      g_debug ("updates session owned by '%s' created", session->sender);
      updates_session->ref = xdp_app_info_get_ref (request->app_info);
      updates_session->installation = xdp_app_info_get_installation (request->app_info);

      start_monitoring (updates_session);
    }

  return (UpdatesSession*)session;
}

static gboolean
create_updates_monitor_done (gpointer data)
{
  g_autoptr(Request) request = data;
  Session *session;
  gboolean should_close_session;
  guint response = 0;
  GVariantBuilder results_builder;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  session = g_object_get_data (G_OBJECT (request), "session");
  SESSION_AUTOLOCK_UNREF (g_object_ref (session));
  g_object_set_data (G_OBJECT (request), "session", NULL);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

 if (request->exported)
    {
      if (!session_export (session, &error))
        {
          g_warning ("Failed to export session: %s", error->message);
          response = 2;
          should_close_session = TRUE;
          goto out;
        }

      should_close_session = FALSE;
      session_register (session);
    }
  else
    {
      should_close_session = TRUE;
    }

  g_variant_builder_add (&results_builder, "{sv}",
                         "session_handle", g_variant_new ("s", session->id));

out:
 if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results_builder));
      request_unexport (request);
    }
  else
    {
      g_variant_builder_clear (&results_builder);
    }

  if (should_close_session)
    session_close (session, FALSE);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_create_update_monitor (XdpUpdates *object,
                              GDBusMethodInvocation *invocation,
                              GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  Session *session;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  session = (Session *)updates_session_new (arg_options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_object_set_data_full (G_OBJECT (request), "session", g_object_ref (session), g_object_unref);

  g_idle_add (create_updates_monitor_done, g_object_ref (request));

  xdp_updates_complete_create_update_monitor (object, invocation, request->id);

  return TRUE;
}

static void
updates_iface_init (XdpUpdatesIface *iface)
{
  iface->handle_install_update = handle_install_update;
  iface->handle_create_update_monitor = handle_create_update_monitor;
}

static void
updates_init (Updates *updates)
{
  xdp_updates_set_version (XDP_UPDATES (updates), 1);
}

static void
updates_class_init (UpdatesClass *klass)
{
}

GDBusInterfaceSkeleton *
updates_create (GDBusConnection *connection,
                const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_access_proxy_new_sync (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         dbus_name,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         NULL,
                                         &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  updates = g_object_new (updates_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (updates);
}
