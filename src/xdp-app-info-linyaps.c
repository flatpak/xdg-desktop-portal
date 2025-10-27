/*
 * Copyright © 2025 UnionTech Software Technology Co., Ltd.
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
 *       ComixHe <heyuming@deepin.org>
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#if HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include "xdp-app-info-linyaps-private.h"

#define LINYAPS_ENGINE_ID "cn.org.linyaps"

#define LINYAPS_METADATA_GROUP_GENERAL "General"
#define LINYAPS_METADATA_KEY_LINYAPS_VERSION "Linyaps-version"
#define LINYAPS_METADATA_GROUP_APPLICATION "Application"
#define LINYAPS_METADATA_KEY_APP_ID "Id"
#define LINYAPS_METADATA_GROUP_INSTANCE "Instance"
#define LINYAPS_METADATA_KEY_INSTANCE_ID "Id"
#define LINYAPS_METADATA_GROUP_CONTEXT "Context"
#define LINYAPS_METADATA_KEY_NETWORK "Network"

struct _XdpAppInfoLinyaps
{
  XdpAppInfo parent;

  GKeyFile *container_info;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoLinyaps, xdp_app_info_linyaps, XDP_TYPE_APP_INFO)

static XdpAppInfo *
xdp_app_info_linyaps_new_testing (const char *sender,
                                  GError    **error)
{
  g_autoptr(XdpAppInfoLinyaps) app_info_linyaps = NULL;
  const char *metadata_path = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  gboolean ret;
  g_autofree gchar *app_id = NULL;
  g_autofree gchar *instance_id = NULL;
  g_autofree gchar *network = NULL;
  XdpAppInfoFlags flags = 0;

  metadata_path = g_getenv ("XDG_DESKTOP_PORTAL_TEST_LINYAPS_METADATA");
  g_assert (metadata_path != NULL);

  metadata = g_key_file_new ();
  ret = g_key_file_load_from_file (metadata,
                                   metadata_path,
                                   G_KEY_FILE_NONE,
                                   NULL);
  g_assert (ret == TRUE);

  app_id = g_key_file_get_string (metadata,
                                  LINYAPS_METADATA_GROUP_APPLICATION,
                                  LINYAPS_METADATA_KEY_APP_ID,
                                  error);
  g_assert (app_id != NULL);

  instance_id = g_key_file_get_string (metadata,
                                       LINYAPS_METADATA_GROUP_INSTANCE,
                                       LINYAPS_METADATA_KEY_INSTANCE_ID,
                                       error);
  g_assert (instance_id != NULL);

  network = g_key_file_get_string (metadata,
                                   LINYAPS_METADATA_GROUP_CONTEXT,
                                   LINYAPS_METADATA_KEY_NETWORK,
                                   error);
  g_assert (network != NULL);

  if (g_strcmp0 (network, "shared") == 0)
    flags |= XDP_APP_INFO_FLAG_HAS_NETWORK;
  flags |= XDP_APP_INFO_FLAG_REQUIRE_GAPPINFO;

  app_info_linyaps = g_initable_new (XDP_TYPE_APP_INFO_LINYAPS,
                                     NULL,
                                     error,
                                     "engine", LINYAPS_ENGINE_ID,
                                     "flags", flags,
                                     "id", app_id,
                                     "instance", instance_id,
                                     "sender", sender,
                                     NULL);

  return XDP_APP_INFO (g_steal_pointer (&app_info_linyaps));
}

/* this implementation refers to flatpak */
static int
open_linyaps_info (int      pid,
                   GError **error)
{
  g_autofree char *root_path = NULL;
  g_autofd int root_fd = -1;
  g_autofd int info_fd = -1;

  root_path = g_strdup_printf ("/proc/%u/root", pid);
  root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (root_fd == -1)
    {
      if (errno == EACCES)
        {
          struct statfs buf;
          if (statfs (root_path, &buf) == 0 &&
              buf.f_type == 0x65735546) /* FUSE_SUPER_MAGIC */
          {
            g_set_error (error, XDP_APP_INFO_ERROR,
                         XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                         "Not a linyaps (fuse rootfs)");
            return -1;
          }
        }

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unable to open %s",
                   root_path);
      return -1;
  }

  info_fd = openat (root_fd, ".linyaps", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (info_fd == -1)
    {
      if (errno == ENOENT)
        {
          g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                       "Not a linyaps (no .linyaps)");
          return -1;
        }

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open application info file");
      return -1;
  }

  return g_steal_fd (&info_fd);
}

static void
xdp_app_info_linyaps_dispose (GObject *object)
{
  G_OBJECT_CLASS (xdp_app_info_linyaps_parent_class)->dispose (object);
}

static void
xdp_app_info_linyaps_class_init (XdpAppInfoLinyapsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = xdp_app_info_linyaps_dispose;
}

static void
xdp_app_info_linyaps_init (XdpAppInfoLinyaps *app_info_linyaps)
{
}

XdpAppInfo *
xdp_app_info_linyaps_new (const char *sender,
                          int         pid,
                          int        *pidfd,
                          GError    **error)
{
  g_autoptr(XdpAppInfoLinyaps) app_info_linyaps = NULL;
  g_autofd int fd = -1;
  g_autoptr(GError) current_error = NULL;
  g_autoptr(GKeyFile) metadata = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  g_autofree gchar *app_id = NULL;
  g_autofree gchar *instance_id = NULL;
  g_autofree gchar *network = NULL;
  XdpAppInfoFlags flags = 0;
  struct stat stat_buf;
  const gchar *test_app_info_kind = NULL;

  test_app_info_kind = g_getenv ("XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND");
  if (test_app_info_kind)
    {
      if (g_strcmp0 (test_app_info_kind, "linyaps") != 0)
        {
          g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                       "Testing requested different AppInfo kind: %s",
                       test_app_info_kind);
          return NULL;
        }

      return xdp_app_info_linyaps_new_testing (sender, error);
    }

  fd = open_linyaps_info (pid, error);
  if (fd == -1)
    return NULL;

  if (fstat (fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open linyaps application info file");
      return NULL;
    }

  mapped = g_mapped_file_new_from_fd (fd, FALSE, &current_error);
  if (mapped == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't map .linyaps file: %s", current_error->message);
      return NULL;
    }

  metadata = g_key_file_new ();
  if (!g_key_file_load_from_data (metadata, g_mapped_file_get_contents (mapped),
                                 g_mapped_file_get_length (mapped),
                                 G_KEY_FILE_NONE, &current_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't load .linyaps file: %s", current_error->message);
      return NULL;
    }

  app_id = g_key_file_get_string (metadata, LINYAPS_METADATA_GROUP_APPLICATION, LINYAPS_METADATA_KEY_APP_ID, error);
  if (app_id == NULL)
    return NULL;

  instance_id = g_key_file_get_string (metadata,LINYAPS_METADATA_GROUP_INSTANCE, LINYAPS_METADATA_KEY_INSTANCE_ID, error);
  if (instance_id == NULL)
    return NULL;

  network = g_key_file_get_string (metadata, LINYAPS_METADATA_GROUP_CONTEXT, LINYAPS_METADATA_KEY_NETWORK, error);
  if (network == NULL)
    return NULL;

  if (g_strcmp0 (network, "shared") == 0)
    flags |= XDP_APP_INFO_FLAG_HAS_NETWORK;

  app_info_linyaps = g_initable_new (XDP_TYPE_APP_INFO_LINYAPS,
                                     NULL,
                                     error,
                                     "engine", LINYAPS_ENGINE_ID,
                                     "flags", flags,
                                     "id", app_id,
                                     "instance", instance_id,
                                     "sender", sender,
                                     NULL);
  if (app_info_linyaps == NULL)
    return NULL;

  app_info_linyaps->container_info = g_steal_pointer (&metadata);

  return XDP_APP_INFO (g_steal_pointer (&app_info_linyaps));
}
