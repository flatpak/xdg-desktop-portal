/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "trash.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Trash Trash;
typedef struct _TrashClass TrashClass;

struct _Trash
{
  XdpTrashSkeleton parent_instance;
};

struct _TrashClass
{
  XdpTrashSkeletonClass parent_class;
};

static Trash *trash;

GType trash_get_type (void) G_GNUC_CONST;
static void trash_iface_init (XdpTrashIface *iface);

G_DEFINE_TYPE_WITH_CODE (Trash, trash, XDP_TYPE_TRASH_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_TRASH, trash_iface_init));

static guint
trash_file (XdpAppInfo *app_info,
            const char *sender,
            int fd)
{
  g_autofree char *path = NULL;
  gboolean writable;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) local_error = NULL;

  path = xdp_app_info_get_path_for_fd (app_info, fd, 0, NULL, &writable, &local_error);

  if (path == NULL)
    {
      g_debug ("Cannot trash file with invalid fd: %s", local_error->message);
      return 0;
    }

  if (!writable)
    {
      g_debug ("Cannot trash file \"%s\": not opened for writing", path);
      return 0;
    }

  file = g_file_new_for_path (path);
  if (!g_file_trash (file, NULL, &local_error))
    {
      g_debug ("Cannot trash file \"%s\": %s", path, local_error->message);
      return 0;
    }

  return 1;
}

static gboolean
handle_trash_file (XdpTrash *object,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList *fd_list,
                   GVariant *arg_fd)
{
  Request *request = request_from_invocation (invocation);
  int idx, fd;
  guint result;

  g_debug ("Handling TrashFile");

  REQUEST_AUTOLOCK (request);

  g_variant_get (arg_fd, "h", &idx);
  fd = g_unix_fd_list_get (fd_list, idx, NULL);

  result = trash_file (request->app_info, request->sender, fd);

  xdp_trash_complete_trash_file (object, invocation, NULL, result);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
trash_iface_init (XdpTrashIface *iface)
{
  iface->handle_trash_file = handle_trash_file;
}

static void
trash_init (Trash *trash)
{
  xdp_trash_set_version (XDP_TRASH (trash), 1);
}

static void
trash_class_init (TrashClass *klass)
{
}

GDBusInterfaceSkeleton *
trash_create (GDBusConnection *connection)
{
  trash = g_object_new (trash_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (trash);
}
