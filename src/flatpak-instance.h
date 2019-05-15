/*
 * Copyright © 2018 Red Hat, Inc
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
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#ifndef __FLATPAK_INSTANCE_H__
#define __FLATPAK_INSTANCE_H__

typedef struct _FlatpakInstance FlatpakInstance;

#include <glib-object.h>

#define FLATPAK_TYPE_INSTANCE flatpak_instance_get_type ()
#define FLATPAK_INSTANCE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_INSTANCE, FlatpakInstance))
#define FLATPAK_IS_INSTANCE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_INSTANCE))

GType flatpak_instance_get_type (void);

struct _FlatpakInstance
{
  GObject parent;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakInstanceClass;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakInstance, g_object_unref)
#endif

GPtrArray *  flatpak_instance_get_all (void);

const char * flatpak_instance_get_id (FlatpakInstance *self);
const char * flatpak_instance_get_app (FlatpakInstance *self);
const char * flatpak_instance_get_arch (FlatpakInstance *self);
const char * flatpak_instance_get_branch (FlatpakInstance *self);
const char * flatpak_instance_get_commit (FlatpakInstance *self);
const char * flatpak_instance_get_runtime (FlatpakInstance *self);
const char * flatpak_instance_get_runtime_commit (FlatpakInstance *self);
int          flatpak_instance_get_pid (FlatpakInstance *self);
int          flatpak_instance_get_child_pid (FlatpakInstance *self);
GKeyFile *   flatpak_instance_get_info (FlatpakInstance *self);

gboolean     flatpak_instance_is_running (FlatpakInstance *self);

#endif /* __FLATPAK_INSTANCE_H__ */
