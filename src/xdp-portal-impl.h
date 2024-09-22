/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <glib.h>

typedef struct {
  char *source;
  char *dbus_name;
  char **interfaces;
  char **use_in;
  int priority;
} XdpPortalImplementation;

void load_installed_portals (gboolean opt_verbose);
void load_portal_configuration (gboolean opt_verbose);
XdpPortalImplementation *find_portal_implementation (const char *interface);
GPtrArray *find_all_portal_implementations (const char *interface);

