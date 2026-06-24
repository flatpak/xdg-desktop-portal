/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
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
 */

#include "config.h"

#include "xdp-entitlements-private.h"
#include "xdp-utils.h"

/* Entitlement metadata table */

typedef struct _XdpEntitlementInfo
{
  const char *name;
  unsigned int version;
} XdpEntitlementInfo;

static const XdpEntitlementInfo xdp_entitlements[] =
{
  [XDP_ENTITLEMENT_NONE] =
    {
      .name = "org.freedesktop.portal.None",
      .version = 0,
    },
};

struct _XdpEntitlements
{
  GObject parent;

  unsigned int version;
  uint64_t granted;
};

G_DEFINE_FINAL_TYPE (XdpEntitlements, xdp_entitlements, G_TYPE_OBJECT)

static void
xdp_entitlements_class_init (XdpEntitlementsClass *klass)
{
}

static void
xdp_entitlements_init (XdpEntitlements *self)
{
}

XdpEntitlements *
xdp_entitlements_new (unsigned int version)
{
  XdpEntitlements *self;

  self = g_object_new (XDP_TYPE_ENTITLEMENTS, NULL);
  self->version = version;

  return self;
}

void
xdp_entitlements_grant (XdpEntitlements *self,
                        XdpEntitlement   entitlement)
{
  g_return_if_fail (XDP_IS_ENTITLEMENTS (self));
  g_return_if_fail (entitlement > XDP_ENTITLEMENT_NONE);

  self->granted |= (1u << entitlement);
}

void
xdp_entitlements_grant_all (XdpEntitlements *self)
{
  g_return_if_fail (XDP_IS_ENTITLEMENTS (self));

  for (size_t i = 1; i < G_N_ELEMENTS (xdp_entitlements); i++)
    self->granted |= (1u << i);
}

gboolean
xdp_entitlements_is_granted (XdpEntitlements *self,
                             XdpEntitlement   entitlement)
{
  unsigned int entitlement_version;

  g_return_val_if_fail (XDP_IS_ENTITLEMENTS (self), FALSE);
  g_return_val_if_fail (entitlement > XDP_ENTITLEMENT_NONE, FALSE);

  entitlement_version = xdp_entitlement_get_version (entitlement);

  if (self->version < entitlement_version)
    return TRUE;

  return (self->granted & (1u << entitlement)) != 0;
}

gboolean
xdp_entitlements_check (XdpEntitlements  *self,
                        GError          **error,
                        ...)
{
  va_list args;
  gboolean res;

  g_return_val_if_fail (XDP_IS_ENTITLEMENTS (self), FALSE);

  va_start (args, error);

  while (TRUE)
    {
      XdpEntitlement entitlement;

      entitlement = va_arg (args, XdpEntitlement);
      if (entitlement == XDP_ENTITLEMENT_NONE)
        {
          res = TRUE;
          break;
        }

      if (!xdp_entitlements_is_granted (self, entitlement))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                       "Missing entitlement %s",
                       xdp_entitlement_get_name (entitlement));
          res = FALSE;
          break;
        }
    }

  va_end (args);
  return res;
}

XdpEntitlement
xdp_entitlement_lookup (const char *entitlement)
{
  for (size_t i = 0; i < G_N_ELEMENTS (xdp_entitlements); i++)
    {
      if (g_strcmp0 (entitlement, xdp_entitlements[i].name) == 0)
        return (XdpEntitlement) i;
    }

  return XDP_ENTITLEMENT_NONE;
}

const char *
xdp_entitlement_get_name (XdpEntitlement entitlement)
{
  g_return_val_if_fail (entitlement > XDP_ENTITLEMENT_NONE, NULL);
  g_return_val_if_fail (entitlement < G_N_ELEMENTS (xdp_entitlements), NULL);

  return xdp_entitlements[entitlement].name;
}

unsigned int
xdp_entitlement_get_version (XdpEntitlement entitlement)
{
  g_return_val_if_fail (entitlement > XDP_ENTITLEMENT_NONE, 0);
  g_return_val_if_fail (entitlement < G_N_ELEMENTS (xdp_entitlements), 0);

  return xdp_entitlements[entitlement].version;
}
