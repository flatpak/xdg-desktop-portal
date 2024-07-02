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

#include "glib-backports.h"
#include "xdp-sealed-fd.h"

#define DESKTOP_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"

gint xdp_mkstempat (int    dir_fd,
                    gchar *tmpl,
                    int    flags,
                    int    mode);

gboolean xdp_is_valid_app_id (const char *string);

char *xdp_get_app_id_from_desktop_id (const char *desktop_id);

gboolean xdp_validate_icon (XdpSealedFd  *icon,
                            char        **out_format,
                            char        **out_size);

typedef void (*XdpPeerDiedCallback) (const char *name);

typedef int XdpFd;
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(XdpFd, close, -1)

void xdp_set_documents_mountpoint (const char *path);
const char * xdp_get_documents_mountpoint (void);
char * xdp_get_alternate_document_path (const char *path, const char *app_id);

void   xdp_connection_track_name_owners  (GDBusConnection       *connection,
                                          XdpPeerDiedCallback    peer_died_cb);

gboolean xdp_variant_contains_key (GVariant *dictionary,
                                   const char *key);

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
char * xdp_spawn (GError             **error,
                  const char          *argv0,
                  ...) G_GNUC_NULL_TERMINATED;
char * xdp_spawn_full (const char * const  *argv,
                       int                  source_fd,
                       int                  target_fd,
                       GError             **error);

char * xdp_canonicalize_filename (const char *path);
gboolean  xdp_has_path_prefix (const char *str,
                               const char *prefix);

#define XDP_EXPORT_TEST XDP_EXPORT
#define XDP_EXPORT __attribute__((visibility("default"))) extern
