/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-method-info.h"

#include <glib.h>

const XdpMethodInfo *
xdp_method_info_find (const char *interface,
                      const char *method)
{
  const XdpMethodInfo *mi = NULL;
  gboolean interface_found = FALSE;

  mi = xdp_method_info_get_all ();
  while (mi->interface != NULL)
    {
      if (g_strcmp0 (interface, mi->interface) == 0)
        {
          interface_found = TRUE;
          if (g_strcmp0 (method, mi->method) == 0)
            return mi;
        }
      else if (interface_found)
        break;
      mi++;
    }

  return NULL;
}
