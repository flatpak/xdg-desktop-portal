/*
 * Copyright © 2024 Red Hat, Inc
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include "xdp-app-info-snap-private.h"

#define SNAP_METADATA_GROUP_INFO "Snap Info"
#define SNAP_METADATA_KEY_INSTANCE_NAME "InstanceName"
#define SNAP_METADATA_KEY_DESKTOP_FILE "DesktopFile"
#define SNAP_METADATA_KEY_NETWORK "HasNetworkStatus"

struct _XdpAppInfoSnap
{
  XdpAppInfo parent;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoSnap, xdp_app_info_snap, XDP_TYPE_APP_INFO)

static void
xdp_app_info_snap_class_init (XdpAppInfoSnapClass *klass)
{
}

static void
xdp_app_info_snap_init (XdpAppInfoSnap *app_info_snap)
{
}

int
_xdp_app_info_snap_parse_cgroup_file (FILE     *f,
                                      gboolean *is_snap)
{
  ssize_t n;
  g_autofree char *id = NULL;
  g_autofree char *controller = NULL;
  g_autofree char *cgroup = NULL;
  size_t id_len = 0, controller_len = 0, cgroup_len = 0;

  g_return_val_if_fail(f != NULL, -1);
  g_return_val_if_fail(is_snap != NULL, -1);

  *is_snap = FALSE;
  do
    {
      n = getdelim (&id, &id_len, ':', f);
      if (n == -1) break;
      n = getdelim (&controller, &controller_len, ':', f);
      if (n == -1) break;
      n = getdelim (&cgroup, &cgroup_len, '\n', f);
      if (n == -1) break;

      /* Only consider the freezer, systemd group or unified cgroup
       * hierarchies */
      if ((strcmp (controller, "freezer:") == 0 ||
           strcmp (controller, "name=systemd:") == 0 ||
           strcmp (controller, ":") == 0) &&
          strstr (cgroup, "/snap.") != NULL)
        {
          *is_snap = TRUE;
          break;
        }
    }
  while (n >= 0);

  if (n < 0 && !feof(f)) return -1;

  return 0;
}

static gboolean
pid_is_snap (pid_t    pid,
             GError **error)
{
  g_autofree char *cgroup_path = NULL;;
  int fd;
  FILE *f = NULL;
  gboolean is_snap = FALSE;
  int err = 0;

  g_return_val_if_fail(pid > 0, FALSE);

  cgroup_path = g_strdup_printf ("/proc/%u/cgroup", (guint) pid);
  fd = open (cgroup_path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd == -1)
    {
      err = errno;
      goto end;
    }

  f = fdopen (fd, "r");
  if (f == NULL)
    {
      err = errno;
      goto end;
    }

  fd = -1; /* fd is now owned by f */

  if (_xdp_app_info_snap_parse_cgroup_file (f, &is_snap) == -1)
    err = errno;

  fclose (f);

end:
  /* Silence ENOENT, treating it as "not a snap" */
  if (err != 0 && err != ENOENT)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                   "Could not parse cgroup info for pid %u: %s", (guint) pid,
                   g_strerror (err));
    }
  return is_snap;
}

gboolean
xdp_is_snap (int        pid,
             gboolean  *is_snap,
             GError   **error)
{
  g_autoptr(GError) local_error = NULL;

  if (!pid_is_snap (pid, &local_error))
    {
      if (local_error && !g_error_matches (local_error, XDP_APP_INFO_ERROR,
                                           XDP_APP_INFO_ERROR_WRONG_APP_KIND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      *is_snap = FALSE;
      return TRUE;
    }
  else
    {
      *is_snap = TRUE;
      return TRUE;
    }
}

XdpAppInfo *
xdp_app_info_snap_new (int      pid,
                       int      pidfd,
                       GError **error)
{
  g_autoptr (XdpAppInfoSnap) app_info_snap = NULL;

  g_autoptr(GError) local_error = NULL;
  g_autofree char *pid_str = NULL;
  g_autofree char *output = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  g_autofree char *snap_name = NULL;
  g_autofree char *snap_id = NULL;
  g_autofree char *desktop_id = NULL;
  g_autoptr(GAppInfo) gappinfo = NULL;
  XdpAppInfoFlags flags = 0;
  gboolean has_network;

  /* Check the process's cgroup membership to fail quickly for non-snaps */
  if (!pid_is_snap (pid, error))
    {
      g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                   "Not a snap (cgroup doesn't contain a snap id)");
      return NULL;
    }

  pid_str = g_strdup_printf ("%u", (guint) pid);
  output = xdp_spawn (error, "snap", "routine", "portal-info", pid_str, NULL);
  if (output == NULL)
    return NULL;

  metadata = g_key_file_new ();
  if (!g_key_file_load_from_data (metadata, output, -1, G_KEY_FILE_NONE, &local_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't read snap info for pid %u: %s", pid, local_error->message);
      return FALSE;
    }

  snap_name = g_key_file_get_string (metadata,
                                     SNAP_METADATA_GROUP_INFO,
                                     SNAP_METADATA_KEY_INSTANCE_NAME,
                                     error);
  if (snap_name == NULL)
    return NULL;

  snap_id = g_strconcat ("snap.", snap_name, NULL);

  desktop_id = g_key_file_get_string (metadata,
                                      SNAP_METADATA_GROUP_INFO,
                                      SNAP_METADATA_KEY_DESKTOP_FILE,
                                      error);
  if (desktop_id == NULL)
    return NULL;

  gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  has_network = g_key_file_get_boolean (metadata,
                                        SNAP_METADATA_GROUP_INFO,
                                        SNAP_METADATA_KEY_NETWORK,
                                        NULL);

  if (has_network)
    flags |= XDP_APP_INFO_FLAG_HAS_NETWORK;

  app_info_snap = g_initable_new (XDP_TYPE_APP_INFO_SNAP,
                                  NULL,
                                  error,
                                  "engine", "io.snapcraft",
                                  NULL);

  xdp_app_info_initialize (XDP_APP_INFO (app_info_snap),
                           snap_id, NULL, pidfd, gappinfo,
                           flags);

  return XDP_APP_INFO (g_steal_pointer (&app_info_snap));
}
