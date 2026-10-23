/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-mount-ns.h"

#include <sys/stat.h>

#include <gio/gio.h>

#include <glnx-mountinfo.h>

struct _XdpMountNs
{
  GPtrArray *entries;      /* (element-type GLnxMountInfo) - sandbox mounts */
  GPtrArray *host_entries; /* (element-type GLnxMountInfo) - host mounts */
};

static int
compare_mount_point_len_desc (gconstpointer a,
                              gconstpointer b)
{
  GLnxMountInfo *info_a = *(GLnxMountInfo **) a;
  GLnxMountInfo *info_b = *(GLnxMountInfo **) b;
  size_t len_a = strlen (glnx_mount_info_get_mount_point (info_a));
  size_t len_b = strlen (glnx_mount_info_get_mount_point (info_b));

  if (len_b > len_a)
    return 1;
  if (len_b < len_a)
    return -1;
  return 0;
}

static GPtrArray *
read_mount_entries (GLnxPidRef *pidref,
                    gboolean    skip_root_mount_point)
{
  g_autoptr(GPtrArray) all = NULL;
  g_autoptr(GPtrArray) filtered = NULL;

  if (pidref)
    all = glnx_mount_info_list_for_pid (pidref, NULL);
  else
    all = glnx_mount_info_list (NULL, NULL);

  if (!all)
    return g_ptr_array_new_with_free_func ((GDestroyNotify) glnx_mount_info_unref);

  if (!skip_root_mount_point)
    {
      g_ptr_array_sort (all, compare_mount_point_len_desc);
      return g_steal_pointer (&all);
    }

  filtered = g_ptr_array_new_with_free_func ((GDestroyNotify) glnx_mount_info_unref);

  for (size_t i = 0; i < all->len; i++)
    {
      GLnxMountInfo *info = g_ptr_array_index (all, i);

      if (g_strcmp0 (glnx_mount_info_get_mount_point (info), "/") == 0)
        continue;

      g_ptr_array_add (filtered, glnx_mount_info_ref (info));
    }

  g_ptr_array_sort (filtered, compare_mount_point_len_desc);

  return g_steal_pointer (&filtered);
}

XdpMountNs *
xdp_mount_ns_new (GLnxPidRef *pidref)
{
  g_autoptr(XdpMountNs) mount_ns = NULL;

  mount_ns = g_new0 (XdpMountNs, 1);

  if (!pidref)
    return g_steal_pointer (&mount_ns);

  mount_ns->entries = read_mount_entries (pidref, /* skip_root_mount_point= */ TRUE);
  mount_ns->host_entries = read_mount_entries (NULL, /* skip_root_mount_point= */ FALSE);

  return g_steal_pointer (&mount_ns);
}

void
xdp_mount_ns_free (XdpMountNs *mount_ns)
{
  if (!mount_ns)
    return;

  g_clear_pointer (&mount_ns->entries, g_ptr_array_unref);
  g_clear_pointer (&mount_ns->host_entries, g_ptr_array_unref);
  g_free (mount_ns);
}

static size_t
count_path_components (const char *path)
{
  size_t count = 0;

  for (; *path; path++)
    {
      if (*path == '/')
        count++;
    }

  return count;
}

static char *
resolve_via_host_mounts (XdpMountNs    *mount_ns,
                         GLnxMountInfo *sandbox_entry,
                         const char    *fs_path,
                         struct stat   *st_buf)
{
  g_autofree char *best = NULL;
  size_t best_depth = G_MAXSIZE;
  dev_t sandbox_dev = glnx_mount_info_get_devno (sandbox_entry);

  for (size_t i = 0; i < mount_ns->host_entries->len; i++)
    {
      GLnxMountInfo *host_entry = g_ptr_array_index (mount_ns->host_entries, i);
      g_autofree char *candidate = NULL;
      struct stat candidate_st;
      const char *host_root = glnx_mount_info_get_root (host_entry);
      const char *host_mount_point = glnx_mount_info_get_mount_point (host_entry);
      size_t host_root_len = strlen (host_root);
      size_t host_mount_point_len = strlen (host_mount_point);
      const char *fs_remainder;
      size_t depth;

      if (glnx_mount_info_get_devno (host_entry) != sandbox_dev)
        continue;

      if (host_root_len == 1 && host_root[0] == '/')
        {
          fs_remainder = fs_path;
        }
      else
        {
          if (!g_str_has_prefix (fs_path, host_root))
            continue;
          if (fs_path[host_root_len] != '/' &&
              fs_path[host_root_len] != '\0')
            continue;

          fs_remainder = fs_path + host_root_len;
        }

      if (host_mount_point_len == 1 && host_mount_point[0] == '/')
        candidate = g_strdup (fs_remainder);
      else
        candidate = g_strconcat (host_mount_point, fs_remainder, NULL);

      if (st_buf)
        {
          if (stat (candidate, &candidate_st) < 0)
            continue;

          if (candidate_st.st_dev != st_buf->st_dev ||
              candidate_st.st_ino != st_buf->st_ino)
            continue;
        }

      depth = count_path_components (candidate);
      if (depth < best_depth)
        {
          g_clear_pointer (&best, g_free);
          best = g_steal_pointer (&candidate);
          best_depth = depth;
        }
    }

  return g_steal_pointer (&best);
}

static char *
resolve_path (XdpMountNs  *mount_ns,
              const char  *path,
              struct stat *st_buf)
{
  if (!mount_ns->entries || !mount_ns->host_entries)
    return NULL;

  for (size_t i = 0; i < mount_ns->entries->len; i++)
    {
      GLnxMountInfo *entry = g_ptr_array_index (mount_ns->entries, i);
      g_autofree char *fs_path = NULL;
      g_autofree char *result = NULL;
      const char *mount_point = glnx_mount_info_get_mount_point (entry);
      const char *root = glnx_mount_info_get_root (entry);
      size_t mount_point_len = strlen (mount_point);
      size_t root_len = strlen (root);
      const char *remainder;

      if (!g_str_has_prefix (path, mount_point))
        continue;

      if (path[mount_point_len] != '/' &&
          path[mount_point_len] != '\0')
        continue;

      remainder = path + mount_point_len;

      if (root_len == 1 && root[0] == '/')
        fs_path = g_strdup (remainder[0] ? remainder : "/");
      else
        fs_path = g_strconcat (root, remainder, NULL);

      result = resolve_via_host_mounts (mount_ns, entry, fs_path, st_buf);
      if (result)
        return g_steal_pointer (&result);
    }

  return NULL;
}

char *
xdp_mount_ns_remap_path (XdpMountNs  *mount_ns,
                         const char  *path,
                         struct stat *st_buf)
{
  struct stat host_st;

  if (stat (path, &host_st) == 0 &&
      host_st.st_dev == st_buf->st_dev &&
      host_st.st_ino == st_buf->st_ino)
    return g_strdup (path);

  return resolve_path (mount_ns, path, st_buf);
}

char *
xdp_mount_ns_remap_path_unverified (XdpMountNs *mount_ns,
                                    const char *path)
{
  char *resolved;

  resolved = resolve_path (mount_ns, path, NULL);
  if (resolved)
    return resolved;

  return g_strdup (path);
}
