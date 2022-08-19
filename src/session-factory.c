/*
 * Copyright © 2022 Red Hat, Inc
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
 */

#include "config.h"

#include "session-factory.h"

#include "request.h"
#include "session.h"
#include "xdp-dbus.h"

typedef struct _SessionFactory SessionFactory;
typedef struct _SessionFactoryClass SessionFactoryClass;

struct _SessionFactory
{
    XdpDbusSessionFactorySkeleton parent_instance;

};

struct _SessionFactoryClass
{
    XdpDbusSessionFactorySkeletonClass parent_class;
};

static SessionFactory *session_factory;

GType session_factory_get_type (void) G_GNUC_CONST;
static void session_factory_iface_init (XdpDbusSessionFactoryIface *iface);

G_DEFINE_TYPE_WITH_CODE (SessionFactory, session_factory,
                         XDP_DBUS_TYPE_SESSION_FACTORY_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SESSION_FACTORY,
                                                session_factory_iface_init));

static gboolean
handle_create_session (XdpDbusSessionFactory *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  Session *session = NULL;
  guint response = 2;
  GVariantBuilder results_builder;

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  // FIXME: actually implement the session creation
#if 0
  session = session_new (arg_options, request, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
#endif
  response = 0;
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&results_builder, "{sv}",
                         "session_handle", g_variant_new ("s", "token0")); // FIXME "session->id"));

  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  response,
                                  g_variant_builder_end (&results_builder));
  request_unexport (request);

  /* FIXME: we can complete the request right here */
  xdp_dbus_session_factory_complete_create_session (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
session_factory_iface_init (XdpDbusSessionFactoryIface *iface)
{
  iface->handle_create_session = handle_create_session;
}

static void
session_factory_init (SessionFactory *f)
{
  xdp_dbus_session_factory_set_version (XDP_DBUS_SESSION_FACTORY (f), 1);
}

static void
session_factory_finalize (GObject *object)
{
  G_OBJECT_CLASS (session_factory_parent_class)->finalize (object);
}

static void
session_factory_class_init (SessionFactoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = session_factory_finalize;
}

GDBusInterfaceSkeleton *
session_factory_create(GDBusConnection *connection)
{
  session_factory = g_object_new (session_factory_get_type (), NULL);
  return G_DBUS_INTERFACE_SKELETON (session_factory);
}
