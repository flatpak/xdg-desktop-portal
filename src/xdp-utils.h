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

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <errno.h>

#ifndef G_DBUS_METHOD_INVOCATION_HANDLED
#define G_DBUS_METHOD_INVOCATION_HANDLED TRUE
#define G_DBUS_METHOD_INVOCATION_UNHANDLED FALSE
#endif

#define DESKTOP_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_KEY_APP_PATH "app-path"
#define FLATPAK_METADATA_KEY_RUNTIME_PATH "runtime-path"
#define FLATPAK_METADATA_KEY_INSTANCE_ID "instance-id"

#define SNAP_METADATA_GROUP_INFO "Snap Info"
#define SNAP_METADATA_KEY_INSTANCE_NAME "InstanceName"
#define SNAP_METADATA_KEY_DESKTOP_FILE "DesktopFile"
#define SNAP_METADATA_KEY_NETWORK "HasNetworkStatus"

gint xdp_mkstempat (int    dir_fd,
                    gchar *tmpl,
                    int    flags,
                    int    mode);

gboolean xdp_is_valid_app_id (const char *string);

typedef void (*XdpPeerDiedCallback) (const char *name);

typedef struct _XdpAppInfo XdpAppInfo;

typedef int XdpFd;
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(XdpFd, close, -1)

XdpAppInfo *xdp_app_info_ref             (XdpAppInfo  *app_info);
void        xdp_app_info_unref           (XdpAppInfo  *app_info);
const char *xdp_app_info_get_id          (XdpAppInfo  *app_info);
char *      xdp_app_info_get_instance    (XdpAppInfo  *app_info);
gboolean    xdp_app_info_is_host         (XdpAppInfo  *app_info);
gboolean    xdp_app_info_supports_opath  (XdpAppInfo  *app_info);
char *      xdp_app_info_remap_path      (XdpAppInfo  *app_info,
                                          const char  *path);
gboolean    xdg_app_info_map_pids        (XdpAppInfo  *app_info,
					  pid_t       *pids,
					  guint        n_pids,
					  GError     **error);
gboolean    xdg_app_info_pidfds_to_pids (XdpAppInfo  *app_info,
                                         const int   *fds,
                                         pid_t       *pids,
                                         gint         count,
                                         GError     **error);
char *      xdp_app_info_get_path_for_fd (XdpAppInfo  *app_info,
                                          int          fd,
                                          int          require_st_mode,
                                          struct stat *st_buf,
                                          gboolean    *writable_out,
                                          GError     **error);
gboolean    xdp_app_info_has_network     (XdpAppInfo  *app_info);
XdpAppInfo *xdp_get_app_info_from_pid    (pid_t        pid,
                                          GError     **error);
GAppInfo *  xdp_app_info_load_app_info   (XdpAppInfo *app_info);
char **     xdp_app_info_rewrite_commandline (XdpAppInfo *app_info,
                                              const char *const *commandline);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdpAppInfo, xdp_app_info_unref)

void  xdp_set_documents_mountpoint    (const char *path);
char *xdp_get_alternate_document_path (const char *path, const char *app_id);

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

#define XDP_AUTOLOCK(name) \
  g_autoptr(GMutexLocker) G_PASTE (name ## locker, __LINE__) = \
    g_mutex_locker_new (&G_LOCK_NAME (name)); \
  (void) G_PASTE (name ## locker, __LINE__);


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

/* exposed for the benefit of tests */
int _xdp_parse_cgroup_file (FILE     *f,
                            gboolean *is_snap);


#if !GLIB_CHECK_VERSION (2, 58, 0)
static inline gboolean
g_hash_table_steal_extended (GHashTable    *hash_table,
                             gconstpointer  lookup_key,
                             gpointer      *stolen_key,
                             gpointer      *stolen_value)
{
  if (g_hash_table_lookup_extended (hash_table, lookup_key, stolen_key, stolen_value))
    {
      g_hash_table_steal (hash_table, lookup_key);
      return TRUE;
    }
  else
      return FALSE;
}
#endif
