/*
 * Copyright © 2023 Red Hat, Inc
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
 */

#pragma once

#include <glib.h>

typedef struct {
  const char *interface;
  const char *method;
  gboolean uses_request;
  int option_arg;
} XdpMethodInfo;

const XdpMethodInfo *
xdp_method_info_find (const char *interface,
                      const char *method);

const XdpMethodInfo *
xdp_method_info_get_all (void);

unsigned int
xdp_method_info_get_count (void);
