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

#include <gio/gio.h>

#define DESKTOP_PORTAL_OBJECT_PATH "/org/freedestkop/portal/desktop"

char * xdp_invocation_lookup_app_id_sync (GDBusMethodInvocation *invocation,
                                          GCancellable          *cancellable,
                                          GError               **error);
void   xdp_connection_track_name_owners  (GDBusConnection       *connection);

typedef struct {
  const char *key;
  const GVariantType *type;
} XdpOptionKey;

void xdp_filter_options (GVariant *options_in,
                         GVariantBuilder *options_out,
                         XdpOptionKey *supported_options,
                         int n_supported_options);
