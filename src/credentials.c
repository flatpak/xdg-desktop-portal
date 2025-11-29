/*
 * Copyright © 2025 Isaiah Inuwa <isaiah.inuwa@gmail.com>
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
 *       Isaiah Inuwa <isaiah.inuwa@gmail.com>
 */

#include "config.h"

#include <gio/gunixfdlist.h>
#include <stdint.h>

#include "glib.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

#include "credentials.h"

typedef struct _CredentialsX CredentialsX;
typedef struct _CredentialsXClass CredentialsXClass;

struct _CredentialsX
{
  XdpDbusCredentialsXSkeleton parent_instance;

  XdpDbusImplCredentialsX *impl;
};

struct _CredentialsXClass
{
  XdpDbusCredentialsXSkeletonClass parent_class;
};


GType credentials_x_get_type (void) G_GNUC_CONST;
static void credentials_x_iface_init (XdpDbusCredentialsXIface *iface);

G_DEFINE_TYPE_WITH_CODE (CredentialsX,
                         credentials_x,
                         XDP_DBUS_TYPE_CREDENTIALS_X_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_CREDENTIALS_X,
                                                credentials_x_iface_init))


G_DEFINE_AUTOPTR_CLEANUP_FUNC (CredentialsX, g_object_unref)

static gboolean
handle_create_credential(XdpDbusCredentialsX *object,
                        GDBusMethodInvocation *invocation,
                        GVariant *arg_request
)
{
    return FALSE;
}

static gboolean
handle_get_credential(XdpDbusCredentialsX *object,
                      GDBusMethodInvocation *invocation,
                      GVariant *arg_request)
{
    return FALSE;
}

static gboolean
handle_get_client_capabilities(XdpDbusCredentialsX *object,
                               GDBusMethodInvocation *invocation)
{
    return FALSE;
}

static void
credentials_x_iface_init (XdpDbusCredentialsXIface *iface)
{
  iface->handle_create_credential = handle_create_credential;

  iface->handle_get_credential = handle_get_credential;
  iface->handle_get_client_capabilities = handle_get_client_capabilities;
}

static void
credentials_x_dispose (GObject *object)
{
  CredentialsX *credentials_x = (CredentialsX *) object;

  g_clear_object (&credentials_x->impl);

  G_OBJECT_CLASS (credentials_x_parent_class)->dispose (object);
}

static void
credentials_x_init (CredentialsX *clipboard)
{
}

static void
credentials_x_class_init (CredentialsXClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = credentials_x_dispose;
}

static CredentialsX *
credentials_new (XdpDbusImplCredentialsX *impl)
{
  CredentialsX *credentials;

  credentials = g_object_new (credentials_x_get_type (), NULL);
  credentials->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (credentials->impl), G_MAXINT);

  xdp_dbus_credentials_x_set_version (XDP_DBUS_CREDENTIALS_X (credentials), 1);

  /*
  g_signal_connect_object (credentials->impl, "selection-transfer",
                           G_CALLBACK (selection_transfer_cb),
                           impl,
                           G_CONNECT_DEFAULT);

  g_signal_connect_object (credentials->impl, "selection-owner-changed",
                           G_CALLBACK (selection_owner_changed_cb),
                           impl,
                           G_CONNECT_DEFAULT);
    */

  return credentials;
}


void
init_credentials (XdpContext *context)
{
  g_autoptr(CredentialsX) credentials = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplCredentialsX) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, CREDENTIALS_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_credentials_x_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 impl_config->dbus_name,
                                                 DESKTOP_DBUS_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create credentials: %s", error->message);
      return;
    }

  credentials = credentials_new (impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&credentials)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}