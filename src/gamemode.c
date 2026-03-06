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
 *       Christian J. Kellner <christian@kellner.me>
 */

#include "config.h"

#define _GNU_SOURCE 1
#include <gio/gunixfdlist.h>
#include <errno.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-utils.h"

#include "gamemode.h"

/* well known names*/
#define GAMEMODE_BACKEND_DBUS_NAME "com.feralinteractive.GameMode"
#define GAMEMODE_BACKEND_DBUS_IFACE "com.feralinteractive.GameMode"
#define GAMEMODE_BACKEND_DBUS_PATH "/com/feralinteractive/GameMode"

/* */
typedef struct _GameMode GameMode;
typedef struct _GameModeClass GameModeClass;

static gboolean handle_query_status (XdpDbusGameMode *object,
                                     GDBusMethodInvocation *invocation,
                                     gint pid);

static gboolean handle_register_game (XdpDbusGameMode *object,
                                      GDBusMethodInvocation *invocation,
                                      gint pid);

static  gboolean handle_unregister_game (XdpDbusGameMode *object,
                                         GDBusMethodInvocation *invocation,
                                         gint pid);

static gboolean handle_query_status_by_pid (XdpDbusGameMode *object,
                                            GDBusMethodInvocation *invocation,
                                            gint target,
                                            gint requester);

static gboolean handle_register_game_by_pid (XdpDbusGameMode *object,
                                             GDBusMethodInvocation *invocation,
                                             gint target,
                                             gint requester);

static gboolean handle_unregister_game_by_pid (XdpDbusGameMode *object,
                                               GDBusMethodInvocation *invocation,
                                               gint target,
                                               gint requester);

static gboolean handle_query_status_by_pidfd (XdpDbusGameMode *object,
                                              GDBusMethodInvocation *invocation,
                                              GUnixFDList *fd_list,
                                              GVariant *arg_target,
                                              GVariant *arg_requester);

static gboolean handle_register_game_by_pidfd (XdpDbusGameMode *object,
                                               GDBusMethodInvocation *invocation,
                                               GUnixFDList *fd_list,
                                               GVariant *arg_target,
                                               GVariant *arg_requester);

static gboolean handle_unregister_game_by_pidfd (XdpDbusGameMode *object,
                                                 GDBusMethodInvocation *invocation,
                                                 GUnixFDList *fd_list,
                                                 GVariant *arg_target,
                                                 GVariant *arg_requester);

/* gobject  */

struct _GameMode
{
  XdpDbusGameModeSkeleton parent_instance;

  /*  */
  GDBusProxy *client;
};

struct _GameModeClass
{
  XdpDbusGameModeSkeletonClass parent_class;
};

GType game_mode_get_type (void) G_GNUC_CONST;
static void game_mode_iface_init (XdpDbusGameModeIface *iface);

G_DEFINE_TYPE_WITH_CODE (GameMode, game_mode, XDP_DBUS_TYPE_GAME_MODE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_GAME_MODE,
                                                game_mode_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GameMode, g_object_unref)

static void
game_mode_iface_init (XdpDbusGameModeIface *iface)
{
  iface->handle_query_status = handle_query_status;
  iface->handle_register_game = handle_register_game;
  iface->handle_unregister_game = handle_unregister_game;

  iface->handle_query_status_by_pid = handle_query_status_by_pid;
  iface->handle_register_game_by_pid = handle_register_game_by_pid;
  iface->handle_unregister_game_by_pid = handle_unregister_game_by_pid;

  iface->handle_query_status_by_pidfd = handle_query_status_by_pidfd;
  iface->handle_register_game_by_pidfd = handle_register_game_by_pidfd;
  iface->handle_unregister_game_by_pidfd = handle_unregister_game_by_pidfd;
}

static void
game_mode_dispose (GObject *object)
{
  GameMode *gamemode = (GameMode *) object;

  g_clear_object (&gamemode->client);

  G_OBJECT_CLASS (game_mode_parent_class)->dispose (object);
}

static void
game_mode_init (GameMode *gamemode)
{
}

static void
game_mode_class_init (GameModeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = game_mode_dispose;
}

/* internal helpers */

static gboolean
game_mode_is_allowed_for_app (const char *app_id, GError **error)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GError) err = NULL;
  const char **stored;
  gboolean ok;

  ok = xdp_dbus_impl_permission_store_call_lookup_sync (xdp_get_permission_store (),
                                                        GAMEMODE_PERMISSION_TABLE,
                                                        GAMEMODE_PERMISSION_ID,
                                                        &perms,
                                                        &data,
                                                        NULL,
                                                        &err);

  if (!ok)
    {
      g_dbus_error_strip_remote_error (err);
      g_debug ("No gamemode permissions found: %s", err->message);
      g_clear_error (&err);
    }
  else if (perms != NULL && g_variant_lookup (perms, app_id, "^a&s", &stored))
    {
      g_autofree char *as_str = NULL;
      gboolean allowed;

      as_str = g_strjoinv (" ", (char **)stored);
      g_debug ("GameMode permissions for %s: %s", app_id, as_str);

      allowed = !g_strv_contains (stored, "no");

      if (!allowed)
        g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                     "GameMode is not allowed for %s", app_id);

      return allowed;
    }

  g_debug ("No gamemode permissions stored for %s: allowing", app_id);

  return TRUE;
}

static gboolean
check_pids(const pid_t *pids, gint count, GError **error)
{

  for (gint i = 0; i < count; i++) {
    if (pids[i] == 0) {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "pid %d is invalid (0)", i);
      return FALSE;
    }
  }

  return TRUE;
}

/* generic dbus call handling */

typedef struct CallData_ {
  GDBusMethodInvocation *inv;
  XdpAppInfo *app_info;
  GDBusProxy *client;

  char *method;

  int      ids[2];
  guint    n_ids;

  GUnixFDList *fdlist;

} CallData;

static CallData *
call_data_new (GDBusMethodInvocation *inv,
               XdpAppInfo            *app_info,
               const char            *method,
               GDBusProxy            *client)
{
  CallData *call;

  call = g_slice_new0 (CallData);

  call->inv = g_object_ref (inv);
  call->app_info = g_object_ref (app_info);
  call->method = g_strdup (method);
  call->client = g_object_ref (client);

  return call;
}

static void
call_data_free (gpointer data)
{
  CallData *call = data;
  if (call == NULL)
    return;

  g_clear_object (&call->inv);
  g_clear_object (&call->app_info);
  g_clear_pointer (&call->method, g_free);
  g_clear_object (&call->fdlist);
  g_clear_object (&call->client);

  g_slice_free (CallData, call);
}

static void
handle_call_thread (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) res = NULL;
  GUnixFDList *fdlist = NULL;
  GVariant *params;
  const char *app_id;
  CallData *call;
  gboolean ok;
  gint r;

  call = (CallData *) task_data;
  app_id = xdp_app_info_get_id (call->app_info);

  if (!game_mode_is_allowed_for_app (app_id, &error))
    {
      g_dbus_method_invocation_return_gerror (call->inv, error);
      return;
    }

  /* if we don't have a list of fds, we got pids and need to map them */
  if (call->fdlist == NULL)
    {
      pid_t pids[2] = {0, };
      guint n_pids;
      ino_t pidns_id;

      n_pids = call->n_ids;

      for (guint i = 0; i < n_pids; i++)
        pids[0] = (pid_t) call->ids[i];

      if (!xdp_app_info_get_pidns (call->app_info, &pidns_id, &error))
        {
          g_prefix_error (&error, "Could not get pidns: ");
          g_warning ("GameMode error: %s", error->message);
          g_dbus_method_invocation_return_gerror (call->inv, error);
          return;
        }

      if (pidns_id != 0 && !xdp_map_pids (pidns_id, pids, n_pids, &error))
        {
          g_prefix_error (&error, "Could not map pids: ");
          g_warning ("GameMode error: %s", error->message);
          g_dbus_method_invocation_return_gerror (call->inv, error);
          return;
        }

      if (n_pids == 1)
        params = g_variant_new ("(i)", (gint32) pids[0]);
      else
        params = g_variant_new ("(ii)", (gint32) pids[0], (gint32) pids[1]);

    }
  else
    {
      pid_t pids[2] = {0, };
      const int *fds;
      gint n_pids;

      fdlist = call->fdlist;

      /* verify fds are actually pidfds */
      fds = g_unix_fd_list_peek_fds (fdlist, &n_pids);

      ok = xdp_pidfds_to_pids (fds, pids, n_pids, &error);

      if (!ok || !check_pids (pids, n_pids, &error))
        {
          g_warning ("Pidfd verification error: %s", error->message);
          g_dbus_method_invocation_return_error (call->inv,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "failed to verify fds as pidfds: %s",
                                                 error->message);
          return;
        }

      params = g_variant_new ("(hh)", 0, 1);
    }

  res = g_dbus_proxy_call_with_unix_fd_list_sync (G_DBUS_PROXY (call->client),
                                                  call->method,
                                                  params,
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  fdlist,
                                                  NULL,
                                                  NULL, /* cancel */
                                                  &error);

  r = -2; /* default to "call got rejected" */
  if (res != NULL)
    g_variant_get (res, "(i)", &r);
  else
    g_debug ("Call to GameMode failed: %s", error->message);

  g_dbus_method_invocation_return_value (call->inv, g_variant_new ("(i)", r));
}

static void
handle_call_in_thread_fds (XdpDbusGameMode       *object,
                           const char            *method,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList           *fdlist)
{
  GameMode *gamemode = (GameMode *)object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GTask) task = NULL;
  CallData *call_data;

  if (g_unix_fd_list_get_length (fdlist) != 2)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "File descriptor number is incorrect");
      return;
    }

  call_data = call_data_new (invocation, app_info, method, gamemode->client);
  call_data->fdlist = g_object_ref (fdlist);

  task = g_task_new (object, NULL, NULL, NULL);

  g_task_set_task_data (task, call_data, call_data_free);
  g_task_run_in_thread (task, handle_call_thread);
}

static void
handle_call_in_thread (XdpDbusGameMode       *object,
                       const char            *method,
                       GDBusMethodInvocation *invocation,
                       gint                   target,
                       gint                   requester)
{
  GameMode *gamemode = (GameMode *)object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GTask) task = NULL;
  CallData *call_data;

  call_data = call_data_new (invocation, app_info, method, gamemode->client);

  call_data->ids[0] = target;
  call_data->n_ids = 1;

  if (requester != 0)
    {
      call_data->ids[1] = requester;
      call_data->n_ids += 1;
    }

  task = g_task_new (object, NULL, NULL, NULL);

  g_task_set_task_data (task, call_data, call_data_free);
  g_task_run_in_thread (task, handle_call_thread);
}

/* dbus */
static gboolean
handle_query_status (XdpDbusGameMode       *object,
                     GDBusMethodInvocation *invocation,
                     gint                   pid)
{
  handle_call_in_thread (object, "QueryStatus", invocation, pid, 0);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_register_game (XdpDbusGameMode       *object,
                      GDBusMethodInvocation *invocation,
                      gint                   pid)
{
  handle_call_in_thread (object, "RegisterGame", invocation, pid, 0);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static  gboolean
handle_unregister_game (XdpDbusGameMode *object,
                        GDBusMethodInvocation *invocation,
                        gint pid)
{
  handle_call_in_thread (object, "UnregisterGame", invocation, pid, 0);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_query_status_by_pid (XdpDbusGameMode *object,
                            GDBusMethodInvocation *invocation,
                            gint target,
                            gint requester)
{
  handle_call_in_thread (object,
                         "QueryStatusByPID",
                         invocation,
                         target,
                         requester);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_register_game_by_pid (XdpDbusGameMode *object,
                             GDBusMethodInvocation *invocation,
                             gint target,
                             gint requester)
{
  handle_call_in_thread (object,
                         "RegisterGameByPID",
                         invocation,
                         target,
                         requester);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_unregister_game_by_pid (XdpDbusGameMode *object,
                               GDBusMethodInvocation *invocation,
                               gint target,
                               gint requester)
{
  handle_call_in_thread (object,
                         "UnregisterGameByPID",
                         invocation,
                         target,
                         requester);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/* pidfd based APIs */
static gboolean
handle_query_status_by_pidfd (XdpDbusGameMode *object,
                              GDBusMethodInvocation *invocation,
                              GUnixFDList *fd_list,
                              GVariant *arg_target,
                              GVariant *arg_requester)
{
  handle_call_in_thread_fds (object,
                             "QueryStatusByPIDFd",
                             invocation,
                             fd_list);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_register_game_by_pidfd (XdpDbusGameMode *object,
                               GDBusMethodInvocation *invocation,
                               GUnixFDList *fd_list,
                               GVariant *arg_target,
                               GVariant *arg_requester)
{
  handle_call_in_thread_fds (object,
                             "RegisterGameByPIDFd",
                             invocation,
                             fd_list);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_unregister_game_by_pidfd (XdpDbusGameMode *object,
                                 GDBusMethodInvocation *invocation,
                                 GUnixFDList *fd_list,
                                 GVariant *arg_target,
                                 GVariant *arg_requester)
{
  handle_call_in_thread_fds (object,
                             "UnregisterGameByPIDFd",
                             invocation,
                             fd_list);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}


/* properties */
static void
update_active_state (GameMode *gamemode,
                     GVariant *client_count)
{
  gboolean enabled = g_variant_get_int32 (client_count) > 0;
  xdp_dbus_game_mode_set_active (XDP_DBUS_GAME_MODE (gamemode), enabled);
}

static void
update_active_state_from_cache (GameMode   *gamemode,
                                GDBusProxy *proxy)
{
  g_autoptr(GVariant) client_count = NULL;

  client_count = g_dbus_proxy_get_cached_property (proxy, "ClientCount");

  if (client_count != NULL)
    update_active_state (gamemode, client_count);
}

static void
on_client_properties_changed (GDBusProxy  *proxy,
                              GVariant    *changed_properties,
                              char       **invalidated_properties,
                              gpointer     user_data)
{
  GameMode *gamemode = user_data;

  update_active_state_from_cache (gamemode, proxy);
}

static GameMode *
game_mode_new (GDBusProxy *client)
{
  GameMode *gamemode;

  gamemode = g_object_new (game_mode_get_type (), NULL);
  gamemode->client = g_object_ref (client);

  xdp_dbus_game_mode_set_version (XDP_DBUS_GAME_MODE (gamemode), 4);

  g_signal_connect_object (gamemode->client, "g-properties-changed",
                           G_CALLBACK (on_client_properties_changed),
                           gamemode, G_CONNECT_DEFAULT);

  update_active_state_from_cache (gamemode, gamemode->client);

  return gamemode;
}

/* public API */
void
init_game_mode (XdpContext *context)
{
  g_autoptr(GameMode) gamemode = NULL;
  g_autoptr(GDBusProxy) client = NULL;
  GDBusProxyFlags flags;
  g_autoptr(GError) error = NULL;

  flags = G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION |
          G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES;
  client = g_dbus_proxy_new_sync (xdp_context_get_connection (context),
                                  flags,
                                  NULL,
                                  GAMEMODE_BACKEND_DBUS_NAME,
                                  GAMEMODE_BACKEND_DBUS_PATH,
                                  GAMEMODE_BACKEND_DBUS_IFACE,
                                  NULL,
                                  &error);

  if (client == NULL)
    {
      g_warning ("Failed to create GameMode proxy: %s", error->message);
      return;
    }

  gamemode = game_mode_new (client);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&gamemode)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
