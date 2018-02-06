/*
 * Copyright © 2014, 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <gio/gio.h>

#define DESKTOP_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"

char * xdp_get_app_id_from_pid (pid_t pid,
                                GError **error);

typedef void (*XdpPeerDiedCallback) (const char *name);

char * xdp_invocation_lookup_app_id_sync (GDBusMethodInvocation *invocation,
                                          GCancellable          *cancellable,
                                          GError               **error);
void   xdp_connection_track_name_owners  (GDBusConnection       *connection,
                                          XdpPeerDiedCallback    peer_died_cb);

GKeyFile *xdp_invocation_lookup_cached_app_info (GDBusMethodInvocation *invocation);

typedef struct {
  const char *key;
  const GVariantType *type;
} XdpOptionKey;

void xdp_filter_options (GVariant *options_in,
                         GVariantBuilder *options_out,
                         XdpOptionKey *supported_options,
                         int n_supported_options);

typedef enum {
  XDG_DESKTOP_PORTAL_ERROR_FAILED     = 0,
  XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
  XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
  XDG_DESKTOP_PORTAL_ERROR_EXISTS,
  XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
  XDG_DESKTOP_PORTAL_ERROR_CANCELLED,
  XDG_DESKTOP_PORTAL_ERROR_WINDOW_DESTROYED
} XdgDesktopPortalErrorEnum;

#define XDG_DESKTOP_PORTAL_ERROR xdg_desktop_portal_error_quark ()

GQuark  xdg_desktop_portal_error_quark (void);

char *xdp_get_path_for_fd (GKeyFile *app_info,
                           int fd);

static inline void
xdp_auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *
xdp_auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

#define XDP_AUTOLOCK(name) G_GNUC_UNUSED __attribute__((cleanup (xdp_auto_unlock_helper))) GMutex * G_PASTE (auto_unlock, __LINE__) = xdp_auto_lock_helper (&G_LOCK_NAME (name))
