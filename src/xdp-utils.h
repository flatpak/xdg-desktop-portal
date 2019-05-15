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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <errno.h>

#define DESKTOP_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_KEY_APP_PATH "app-path"
#define FLATPAK_METADATA_KEY_RUNTIME_PATH "runtime-path"

gint xdp_mkstempat (int    dir_fd,
                    gchar *tmpl,
                    int    flags,
                    int    mode);

gboolean xdp_is_valid_app_id (const char *string);

typedef void (*XdpPeerDiedCallback) (const char *name);

typedef struct _XdpAppInfo XdpAppInfo;

XdpAppInfo *xdp_app_info_ref             (XdpAppInfo  *app_info);
void        xdp_app_info_unref           (XdpAppInfo  *app_info);
const char *xdp_app_info_get_id          (XdpAppInfo  *app_info);
char *      xdp_app_info_get_ref         (XdpAppInfo  *app_info);
char *      xdp_app_info_get_inst_path   (XdpAppInfo  *app_info);
gboolean    xdp_app_info_is_host         (XdpAppInfo  *app_info);
gboolean    xdp_app_info_supports_opath  (XdpAppInfo  *app_info);
char *      xdp_app_info_remap_path      (XdpAppInfo  *app_info,
                                          const char  *path);
char *      xdp_app_info_get_path_for_fd (XdpAppInfo  *app_info,
                                          int          fd,
                                          int          require_st_mode,
                                          struct stat *st_buf,
                                          gboolean    *writable_out);
gboolean    xdp_app_info_has_network     (XdpAppInfo  *app_info);
XdpAppInfo *xdp_get_app_info_from_pid    (pid_t        pid,
                                          GError     **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdpAppInfo, xdp_app_info_unref)

XdpAppInfo *xdp_invocation_lookup_app_info_sync (GDBusMethodInvocation *invocation,
                                                 GCancellable          *cancellable,
                                                 GError               **error);
void   xdp_connection_track_name_owners  (GDBusConnection       *connection,
                                          XdpPeerDiedCallback    peer_died_cb);


typedef struct {
  const char *key;
  const GVariantType *type;
  gboolean (* validate) (const char *key, GVariant *value, GVariant *options, GError **error);
} XdpOptionKey;

gboolean xdp_filter_options (GVariant *options_in,
                             GVariantBuilder *options_out,
                             XdpOptionKey *supported_options,
                             int n_supported_options,
                             GError **error);

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

static inline int
xdp_steal_fd (int *fdp)
{
  int fd = *fdp;
  *fdp = -1;
  return fd;
}

static inline void
xdp_close_fd (int *fdp)
{
  int errsv;

  g_assert (fdp);

  int fd = xdp_steal_fd (fdp);
  if (fd >= 0)
    {
      errsv = errno;
      if (close (fd) < 0)
        g_assert (errno != EBADF);
      errno = errsv;
    }
}

#define xdp_autofd __attribute__((cleanup(xdp_close_fd)))

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


char *   xdp_quote_argv (const char           *argv[]);
gboolean xdp_spawn      (GFile                *dir,
                         char                **output,
                         GSubprocessFlags      flags,
                         GError              **error,
                         const gchar          *argv0,
                         va_list               ap);
gboolean xdp_spawnv     (GFile                *dir,
                         char                **output,
                         GSubprocessFlags      flags,
                         GError              **error,
                         const gchar * const  *argv);

char * xdp_canonicalize_filename (const char *path);
gboolean  xdp_has_path_prefix (const char *str,
                               const char *prefix);
