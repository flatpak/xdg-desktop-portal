/*
 * Copyright Red Hat
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
 */

#include "config.h"

#include <gio/gio.h>

#include "save-restore.h"
#include "xdp-session.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

struct _XdpSaveRestore
{
  XdpDbusSaveRestoreSkeleton parent_instance;

  XdpDbusImplSaveRestore *impl;
  GCancellable *cancellable;
};

#define XDP_TYPE_SAVE_RESTORE (xdp_save_restore_get_type ())
G_DECLARE_FINAL_TYPE (XdpSaveRestore,
                      xdp_save_restore,
                      XDP, SAVE_RESTORE,
                      XdpDbusSaveRestoreSkeleton)

enum
{
  PROP_0,
  PROP_IMPL,
  PROP_LAST
};

static void xdp_save_restore_iface_init (XdpDbusSaveRestoreIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSaveRestore,
                               xdp_save_restore,
                               XDP_DBUS_TYPE_SAVE_RESTORE_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SAVE_RESTORE,
                                                      xdp_save_restore_iface_init));

struct _XdpSaveRestoreSession
{
  XdpSession parent;

  gboolean closed;
};

#define XDP_TYPE_SAVE_RESTORE_SESSION (xdp_save_restore_session_get_type ())
G_DECLARE_FINAL_TYPE (XdpSaveRestoreSession,
                      xdp_save_restore_session,
                      XDP, SAVE_RESTORE_SESSION_SESSION,
                      XdpSession)

G_DEFINE_TYPE (XdpSaveRestoreSession,
               xdp_save_restore_session,
               xdp_session_get_type ())

static void
xdp_save_restore_session_close (XdpSession *session)
{
  XdpSaveRestoreSession *save_restore_session = XDP_SAVE_RESTORE_SESSION_SESSION (session);

  save_restore_session->closed = TRUE;
  g_debug ("save/restore session owned by '%s' closed", session->sender);
}

static void
xdp_save_restore_session_finalize (GObject *object)
{
  G_OBJECT_CLASS (xdp_save_restore_session_parent_class)->finalize (object);
}

static void
xdp_save_restore_session_init (XdpSaveRestoreSession *session)
{
}

static void
xdp_save_restore_session_class_init (XdpSaveRestoreSessionClass *klass)
{
  GObjectClass *object_class;
  XdpSessionClass *session_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = xdp_save_restore_session_finalize;

  session_class = (XdpSessionClass *)klass;
  session_class->close = xdp_save_restore_session_close;
}

static XdpSession *
xdp_save_restore_session_new (XdpDbusImplSaveRestore *impl,
                              GDBusMethodInvocation  *invocation,
                              GVariant               *options,
                              GError                **error)
{
  XdpSaveRestoreSession *session;
  XdpCall *call;
  const char *sender;

  call = xdp_call_from_invocation (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  session = g_initable_new (XDP_TYPE_SAVE_RESTORE_SESSION, NULL, error,
                            "sender", sender,
                            "app-id", xdp_app_info_get_id (call->app_info),
                            "token", lookup_session_token (options),
                            "connection", g_dbus_method_invocation_get_connection (invocation),
                            "impl-connection", g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                            "impl-dbus-name", g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                            NULL);
  if (session)
    g_debug ("save/restore session owned by '%s' created", sender);

  return XDP_SESSION (session);
}

typedef struct {
  XdpSaveRestore *save_restore;
  GDBusMethodInvocation *invocation;
  XdpSession *session;
} RegisterData;

static void
impl_register_done (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  XdpDbusImplSaveRestore *impl_proxy = XDP_DBUS_IMPL_SAVE_RESTORE (source_object);
  g_autofree RegisterData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) out_restore = NULL;

  if (!xdp_dbus_impl_save_restore_call_register_finish (impl_proxy, &out_restore,
                                                        result, &error))
    {
      xdp_session_close (data->session, TRUE, FALSE);
      g_dbus_method_invocation_return_gerror (data->invocation, error);
      return;
    }

  xdp_dbus_save_restore_complete_register (XDP_DBUS_SAVE_RESTORE (data->save_restore),
                                           data->invocation,
                                           data->session->id,
                                           out_restore);
}

static const XdpOptionKey register_options[] = {
  { "session_handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static gboolean
handle_register (XdpDbusSaveRestore    *object,
                 GDBusMethodInvocation *invocation,
                 GVariant              *arg_options)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (object);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) options = NULL;
  g_autoptr (GVariant) impl_options = NULL;
  g_autofree RegisterData *data = NULL;
  GVariantBuilder options_builder;
  XdpSession *session;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           register_options, G_N_ELEMENTS (register_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  session = xdp_save_restore_session_new (self->impl, invocation, options, &error);
  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_session_export (session, &error))
    {
      xdp_session_close (session, FALSE, FALSE);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_session_register (session);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  data = g_new (RegisterData, 1);
  *data = (RegisterData) {
    .save_restore = self,
    .invocation = invocation,
    .session = session,
  };
  xdp_dbus_impl_save_restore_call_register (self->impl,
                                            session->id,
                                            session->app_id,
                                            g_variant_builder_end (&options_builder),
                                            self->cancellable,
                                            impl_register_done,
                                            g_steal_pointer (&data));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_discarded_instance_ids (XdpDbusSaveRestore    *object,
                               GDBusMethodInvocation *invocation,
                               const char            *session_id,
                               const char *const     *instance_ids)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (object);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_save_restore_call_discarded_instance_ids (self->impl,
                                                          session_id,
                                                          instance_ids,
                                                          self->cancellable,
                                                          NULL, NULL);

  xdp_dbus_save_restore_complete_discarded_instance_ids (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_save_hint_response (XdpDbusSaveRestore    *object,
                           GDBusMethodInvocation *invocation,
                           const char            *session_id)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (object);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_save_restore_call_save_hint_response (self->impl,
                                                      session->id,
                                                      self->cancellable,
                                                      NULL, NULL);

  xdp_dbus_save_restore_complete_save_hint_response (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_save_restore_iface_init (XdpDbusSaveRestoreIface *iface)
{
  iface->handle_register = handle_register;
  iface->handle_discarded_instance_ids = handle_discarded_instance_ids;
  iface->handle_save_hint_response = handle_save_hint_response;
}

static void
xdp_save_restore_init (XdpSaveRestore *save_restore)
{
  xdp_dbus_save_restore_set_version (XDP_DBUS_SAVE_RESTORE (save_restore), 1);

  save_restore->cancellable = g_cancellable_new ();
}

static void
xdp_save_restore_finalize (GObject *object)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_object (&self->impl);

  G_OBJECT_CLASS (xdp_save_restore_parent_class)->finalize (object);
}

static void
save_hint_cb (XdpDbusImplSaveRestore *impl,
              const char             *session_id,
              gpointer                data)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (data);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);
  XdpSaveRestoreSession *save_restore_session = XDP_SAVE_RESTORE_SESSION_SESSION (session);

  if (save_restore_session && !save_restore_session->closed)
    xdp_dbus_save_restore_emit_save_hint (XDP_DBUS_SAVE_RESTORE (self),
                                          session->id);
}

static void
quit_cb (XdpDbusImplSaveRestore *impl,
              const char             *session_id,
              gpointer                data)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (data);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);
  XdpSaveRestoreSession *save_restore_session = XDP_SAVE_RESTORE_SESSION_SESSION (session);

  if (save_restore_session && !save_restore_session->closed)
    xdp_dbus_save_restore_emit_save_hint (XDP_DBUS_SAVE_RESTORE (self),
                                          session->id);
}

static void
xdp_save_restore_set_impl (XdpSaveRestore         *self,
                           XdpDbusImplSaveRestore *new_impl)
{
  if (self->impl == new_impl)
    return;

  if (self->impl)
    {
      g_signal_handlers_disconnect_by_data (self->impl, self);
      g_clear_object (&self->impl);
    }

  self->impl = g_object_ref (new_impl);
  g_signal_connect (new_impl, "save-hint", G_CALLBACK (save_hint_cb), self);
  g_signal_connect (new_impl, "quit", G_CALLBACK (quit_cb), self);
}

static void
xdp_save_restore_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (object);

  switch (prop_id)
    {
    case PROP_IMPL:
      xdp_save_restore_set_impl (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
xdp_save_restore_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  XdpSaveRestore *self = XDP_SAVE_RESTORE (object);

  switch (prop_id)
    {
    case PROP_IMPL:
      g_value_set_object (value, self->impl);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
xdp_save_restore_class_init (XdpSaveRestoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_save_restore_finalize;
  object_class->set_property = xdp_save_restore_set_property;
  object_class->get_property = xdp_save_restore_get_property;

  g_object_class_install_property (object_class, PROP_IMPL,
                                   g_param_spec_object ("impl",
                                                        "impl",
                                                        "impl",
                                                        XDP_DBUS_IMPL_TYPE_SAVE_RESTORE,
                                                        G_PARAM_READWRITE|G_PARAM_CONSTRUCT_ONLY|G_PARAM_STATIC_STRINGS));
}

GDBusInterfaceSkeleton *
save_restore_create (GDBusConnection *connection,
                     const char      *dbus_name)
{
  g_autoptr (XdpDbusImplSaveRestore) impl = NULL;
  g_autoptr(GError) error = NULL;
  XdpSaveRestore *save_restore = NULL;

  impl = xdp_dbus_impl_save_restore_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    dbus_name,
                                                    "/org/freedesktop/portal/desktop",
                                                    NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create save/restore proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  save_restore = g_object_new (XDP_TYPE_SAVE_RESTORE,
                               "impl", impl,
                               NULL);

  return G_DBUS_INTERFACE_SKELETON (save_restore);
}
