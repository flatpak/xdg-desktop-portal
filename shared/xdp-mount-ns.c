/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-mount-ns.h"

#include <sys/stat.h>

#include <gio/gio.h>
#include <sys/sysmacros.h>

#include "xdp-utils.h"

typedef struct _XdpMountNsEntry
{
  char *root;
  size_t root_len;
  char *mount_point;
  size_t mount_point_len;
  dev_t dev;
} XdpMountNsEntry;

struct _XdpMountNs
{
  GPtrArray *entries;      /* XdpMountNsEntry - sandbox mounts */
  GPtrArray *host_entries; /* XdpMountNsEntry - host mounts */
};

/* Decode mountinfo octal escapes (\040 for space, etc.) */
static char *
mountinfo_unescape (const char *str)
{
  g_autoptr(GString) result = g_string_new (NULL);

  for (; *str; str++)
    {
      if (str[0] == '\\' &&
          str[1] >= '0' && str[1] <= '3' &&
          str[2] >= '0' && str[2] <= '7' &&
          str[3] >= '0' && str[3] <= '7')
        {
          g_string_append_c (result,
                             ((str[1] - '0') << 6) |
                             ((str[2] - '0') << 3) |
                             ((str[3] - '0') << 0));
          str += 3;
        }
      else
        {
          g_string_append_c (result, *str);
        }
    }

  return g_string_free_and_steal (g_steal_pointer (&result));
}

static void
mount_ns_entry_free (XdpMountNsEntry *entry)
{
  g_free (entry->root);
  g_free (entry->mount_point);
  g_free (entry);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpMountNsEntry, mount_ns_entry_free)


static int
compare_mount_point_len_desc (gconstpointer a,
                              gconstpointer b)
{
  const XdpMountNsEntry *entry_a = *(const XdpMountNsEntry **) a;
  const XdpMountNsEntry *entry_b = *(const XdpMountNsEntry **) b;

  if (entry_b->mount_point_len > entry_a->mount_point_len)
    return 1;
  if (entry_b->mount_point_len < entry_a->mount_point_len)
    return -1;
  return 0;
}

static GPtrArray *
parse_mountinfo (const char *path,
                 gboolean    skip_root_mount_point)
{
  g_autoptr(GPtrArray) entries = NULL;
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;

  entries =
    g_ptr_array_new_with_free_func ((GDestroyNotify) mount_ns_entry_free);

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return g_steal_pointer (&entries);

  lines = g_strsplit (contents, "\n", -1);

  for (size_t i = 0; lines[i] != NULL; i++)
    {
      g_auto(GStrv) fields = NULL;
      g_autoptr(XdpMountNsEntry) entry = NULL;
      g_autofree char *mount_point = NULL;
      g_autofree char *root = NULL;
      unsigned int major, minor;

      if (lines[i][0] == '\0')
        continue;

      fields = g_strsplit (lines[i], " ", 6);
      if (g_strv_length (fields) < 5)
        continue;

      if (sscanf (fields[2], "%u:%u", &major, &minor) != 2)
        continue;

      mount_point = mountinfo_unescape (fields[4]);

      if (skip_root_mount_point && g_strcmp0 (mount_point, "/") == 0)
        continue;

      root = mountinfo_unescape (fields[3]);

      entry = g_new (XdpMountNsEntry, 1);
      entry->root = g_steal_pointer (&root);
      entry->root_len = strlen (entry->root);
      entry->mount_point = g_steal_pointer (&mount_point);
      entry->mount_point_len = strlen (entry->mount_point);
      entry->dev = makedev (major, minor);

      g_ptr_array_add (entries, g_steal_pointer (&entry));
    }

  g_ptr_array_sort (entries, compare_mount_point_len_desc);

  return g_steal_pointer (&entries);
}

XdpMountNs *
xdp_mount_ns_new (int pidfd)
{
  g_autoptr(XdpMountNs) mount_ns = NULL;
  g_autoptr(GPtrArray) entries = NULL;
  g_autoptr(GPtrArray) host_entries = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *mountinfo_path = NULL;
  pid_t pid;

  mount_ns = g_new0 (XdpMountNs, 1);

  if (pidfd < 0)
    return g_steal_pointer (&mount_ns);

  pid = xdp_pidfd_to_pid (pidfd, &local_error);
  if (pid < 0)
    return g_steal_pointer (&mount_ns);

  mountinfo_path = g_strdup_printf ("/proc/%d/mountinfo", (int) pid);
  entries = parse_mountinfo (mountinfo_path, /* skip_root_mount_point= */ TRUE);
  host_entries =
    parse_mountinfo ("/proc/self/mountinfo", /* skip_root_mount_point= */ FALSE);

  /* Re-check: the process may have died and the pid been recycled */
  g_clear_error (&local_error);
  if (xdp_pidfd_to_pid (pidfd, &local_error) < 0)
    return g_steal_pointer (&mount_ns);

  mount_ns->entries = g_steal_pointer (&entries);
  mount_ns->host_entries = g_steal_pointer (&host_entries);

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
resolve_via_host_mounts (XdpMountNs      *mount_ns,
                         XdpMountNsEntry *sandbox_entry,
                         const char      *fs_path,
                         struct stat     *st_buf)
{
  g_autofree char *best = NULL;
  size_t best_depth = G_MAXSIZE;

  for (size_t i = 0; i < mount_ns->host_entries->len; i++)
    {
      XdpMountNsEntry *host_entry = g_ptr_array_index (mount_ns->host_entries, i);
      g_autofree char *candidate = NULL;
      struct stat candidate_st;
      const char *fs_remainder;
      size_t depth;

      if (host_entry->dev != sandbox_entry->dev)
        continue;

      if (host_entry->root_len == 1 && host_entry->root[0] == '/')
        {
          fs_remainder = fs_path;
        }
      else
        {
          if (!g_str_has_prefix (fs_path, host_entry->root))
            continue;
          if (fs_path[host_entry->root_len] != '/' &&
              fs_path[host_entry->root_len] != '\0')
            continue;

          fs_remainder = fs_path + host_entry->root_len;
        }

      if (host_entry->mount_point_len == 1 && host_entry->mount_point[0] == '/')
        candidate = g_strdup (fs_remainder);
      else
        candidate = g_strconcat (host_entry->mount_point, fs_remainder, NULL);

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
      XdpMountNsEntry *entry = g_ptr_array_index (mount_ns->entries, i);
      g_autofree char *fs_path = NULL;
      g_autofree char *result = NULL;
      const char *remainder;

      if (!g_str_has_prefix (path, entry->mount_point))
        continue;

      if (path[entry->mount_point_len] != '/' &&
          path[entry->mount_point_len] != '\0')
        continue;

      remainder = path + entry->mount_point_len;

      if (entry->root_len == 1 && entry->root[0] == '/')
        fs_path = g_strdup (remainder[0] ? remainder : "/");
      else
        fs_path = g_strconcat (entry->root, remainder, NULL);

      result = resolve_via_host_mounts (mount_ns, entry, fs_path, st_buf);
      if (result)
        return g_steal_pointer (&result);
    }

  return NULL;
}

/*
 * Remap a path from the sandbox mount namespace to the host.
 *
 * The returned path is guaranteed to refer to the same file as
 * @st_buf (verified by dev/ino). Returns NULL if no verified
 * match was found.
 */
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

/*
 * Best-effort remap of a path from the sandbox mount namespace to the
 * host, based on the mount layout. The returned path will correspond
 * to a mount visible to the sandbox, but is not verified against a
 * specific file. Always returns a path.
 */
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
