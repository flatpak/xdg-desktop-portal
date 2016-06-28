/*
 * Copyright © 2014 Red Hat, Inc
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
 */

#ifndef __XDP_UTILS_H__
#define __XDP_UTILS_H__

#include <gio/gio.h>

typedef struct _Request Request;
typedef struct _RequestClass RequestClass;

char * xdp_invocation_lookup_app_id_sync (GDBusMethodInvocation *invocation,
                                          GCancellable          *cancellable,
                                          GError               **error);
void   xdp_connection_track_name_owners  (GDBusConnection       *connection);
void   xdp_register_request              (Request               *request);
void   xdp_unregister_request            (Request               *request);


#endif /* __XDP_UTILS_H__ */
