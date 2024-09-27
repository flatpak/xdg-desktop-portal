/*
 * Copyright © 2023 Red Hat, Inc
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
 */

#include "config.h"

#include <glib.h>

#include "xdp-method-info.h"

const XdpMethodInfo *
xdp_method_info_find (const char *interface,
                      const char *method)
{
  const XdpMethodInfo *mi = NULL;
  gboolean interface_found = FALSE;

  mi = xdp_method_info_get_all ();
  while (mi->interface != NULL)
    {
      if (strcmp (interface, mi->interface) == 0)
        {
          interface_found = TRUE;
          if (strcmp (method, mi->method) == 0)
            return mi;
        }
      else if (interface_found)
        break;
      mi++;
    }

  return NULL;
}
