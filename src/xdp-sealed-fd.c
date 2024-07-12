/*
 * Copyright © 2024 GNOME Foundation Inc.
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
 *       Julian Sparber <jsparber@gnome.org>
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include "xdp-utils.h"
#include "xdp-sealed-fd.h"

static const int required_seals = F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SHRINK;

struct _XdpSealedFd {
  GObject parent_instance;

  int fd;
};

G_DEFINE_FINAL_TYPE (XdpSealedFd, xdp_sealed_fd, G_TYPE_OBJECT)

static void
xdp_sealed_fd_finalize (GObject *object)
{
  XdpSealedFd *sealed_fd = XDP_SEALED_FD (object);

  xdp_close_fd (&sealed_fd->fd);

  G_OBJECT_CLASS (xdp_sealed_fd_parent_class)->finalize (object);
}

static void
xdp_sealed_fd_class_init (XdpSealedFdClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_sealed_fd_finalize;
}

static void
xdp_sealed_fd_init (XdpSealedFd *sealed_fd)
{
  sealed_fd->fd = -1;
}

XdpSealedFd *
xdp_sealed_fd_new_take_memfd (int      memfd,
                              GError **error)
{
  int saved_errno = -1;
  g_autoptr(XdpSealedFd) sealed_fd = NULL;
  xdp_autofd int fd = g_steal_fd (&memfd);
  int seals;

  g_return_val_if_fail (fd != -1, NULL);

  seals = fcntl (fd, F_GET_SEALS);
  if (seals == -1)
   {
      saved_errno = errno;

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "fcntl F_GET_SEALS: %s", g_strerror (saved_errno));
      return NULL;
    }

  /* If the seal seal is set and some required seal is missing report EPERM error directly */
  if ((seals & F_SEAL_SEAL) && (seals & required_seals) != required_seals)
    saved_errno = EPERM;
  else if (fcntl (fd, F_ADD_SEALS, required_seals) == -1)
    saved_errno = errno;

  if (saved_errno != -1)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "fcntl F_ADD_SEALS: %s", g_strerror (saved_errno));
      return NULL;
    }

  sealed_fd = g_object_new (XDP_TYPE_SEALED_FD, NULL);
  sealed_fd->fd = xdp_steal_fd (&fd);

  return g_steal_pointer (&sealed_fd);
}

XdpSealedFd *
xdp_sealed_fd_new_from_bytes (GBytes  *bytes,
                              GError **error)
{
  gconstpointer bytes_data;
  gsize bytes_len;
  xdp_autofd int fd = -1;
  int saved_errno = -1;
  g_autoptr(XdpSealedFd) sealed_fd = NULL;
  gpointer shm;
  g_autoptr(GOutputStream) stream = NULL;

  g_return_val_if_fail (bytes != NULL, NULL);

  fd = memfd_create ("xdp-sealed-fd", MFD_ALLOW_SEALING);
  if (fd == -1)
    {
      int saved_errno;

      saved_errno = errno;
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "memfd_create: %s", g_strerror (saved_errno));
      return NULL;
    }

  bytes_data = g_bytes_get_data (bytes, &bytes_len);

  if (ftruncate (fd, bytes_len) == -1)
    {
      int saved_errno;

      saved_errno = errno;
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "ftruncate: %s", g_strerror (saved_errno));
      return NULL;
    }

  shm = mmap (NULL, bytes_len, PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm == MAP_FAILED)
    {
      int saved_errno;

      saved_errno = errno;
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "mmap: %s", g_strerror (saved_errno));
      return NULL;
    }

  memcpy (shm, bytes_data, bytes_len);

  if (munmap (shm, bytes_len) == -1)
    {
      int saved_errno;

      saved_errno = errno;
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "munmap: %s", g_strerror (saved_errno));
      return NULL;
    }

  if (fcntl (fd, F_ADD_SEALS, required_seals) == -1)
    {
      saved_errno = errno;
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "fcntl F_ADD_SEALS: %s", g_strerror (saved_errno));
      return NULL;
    }

  sealed_fd = g_object_new (XDP_TYPE_SEALED_FD, NULL);
  sealed_fd->fd = xdp_steal_fd (&fd);

  return g_steal_pointer (&sealed_fd);
}

XdpSealedFd *
xdp_sealed_fd_new_from_handle (GVariant     *handle,
                               GUnixFDList  *fd_list,
                               GError      **error)
{
  int fd_id;
  xdp_autofd int fd = -1;

  g_return_val_if_fail (g_variant_is_of_type (handle, G_VARIANT_TYPE_HANDLE), NULL);
  g_return_val_if_fail (G_IS_UNIX_FD_LIST (fd_list), NULL);
  g_return_val_if_fail (g_unix_fd_list_get_length (fd_list) > 0, NULL);

  fd_id = g_variant_get_handle (handle);
  fd = g_unix_fd_list_get (fd_list, fd_id, error);
  if (fd == -1)
    return NULL;

  return xdp_sealed_fd_new_take_memfd (xdp_steal_fd (&fd), error);
}

int
xdp_sealed_fd_get_fd (XdpSealedFd *sealed_fd)
{
  g_return_val_if_fail (XDP_IS_SEALED_FD (sealed_fd), -1);

  return sealed_fd->fd;
}

int
xdp_sealed_fd_dup_fd (XdpSealedFd *sealed_fd)
{
  g_return_val_if_fail (XDP_IS_SEALED_FD (sealed_fd), -1);

  return dup (sealed_fd->fd);
}
