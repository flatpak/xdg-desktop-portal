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

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

#include "save-restore.h"

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
                      XDP, SAVE_RESTORE_SESSION,
                      XdpSession)

G_DEFINE_TYPE (XdpSaveRestoreSession,
               xdp_save_restore_session,
               xdp_session_get_type ())

static void
xdp_save_restore_session_close (XdpSession *session)
{
  XdpSaveRestoreSession *save_restore_session = XDP_SAVE_RESTORE_SESSION (session);

  save_restore_session->closed = TRUE;
  g_debug ("save/restore session owned by '%s' closed", session->sender);
}

static void
xdp_save_restore_session_init (XdpSaveRestoreSession *session)
{
}

static void
xdp_save_restore_session_class_init (XdpSaveRestoreSessionClass *klass)
{
  XdpSessionClass *session_class;

  session_class = (XdpSessionClass *)klass;
  session_class->close = xdp_save_restore_session_close;
}

static XdpSession *
xdp_save_restore_session_new (XdpSaveRestore         *save_restore,
                              GDBusMethodInvocation  *invocation,
                              GVariant               *options,
                              GError                **error)
{
  XdpSaveRestoreSession *session;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  GDBusProxy *impl = G_DBUS_PROXY (save_restore->impl);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  session = g_initable_new (XDP_TYPE_SAVE_RESTORE_SESSION, NULL, error,
                            "sender", sender,
                            "app-id", xdp_app_info_get_id (app_info),
                            "token", lookup_session_token (options),
                            "connection", g_dbus_method_invocation_get_connection (invocation),
                            "impl-connection", g_dbus_proxy_get_connection (impl),
                            "impl-dbus-name", g_dbus_proxy_get_name (impl),
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

static RegisterData *
register_data_new (XdpSaveRestore        *save_restore,
                   GDBusMethodInvocation *invocation,
                   XdpSession            *session)
{
  RegisterData *data;

  data = g_new0 (RegisterData, 1);
  data->save_restore = g_object_ref (save_restore);
  data->invocation = g_object_ref (invocation);
  data->session = g_object_ref (session);

  return data;
}

static void
register_data_free (RegisterData *data)
{
  g_clear_object (&data->save_restore);
  g_clear_object (&data->invocation);
  g_clear_object (&data->session);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RegisterData, register_data_free)

static void
impl_register_done (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  XdpDbusImplSaveRestore *impl = XDP_DBUS_IMPL_SAVE_RESTORE (source_object);
  g_autoptr(RegisterData) data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_restore = NULL;

  if (!xdp_dbus_impl_save_restore_call_register_finish (impl, &out_restore,
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
  XdpSaveRestore *save_restore = XDP_SAVE_RESTORE (object);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) impl_options = NULL;
  GVariantBuilder options_builder;
  XdpSession *session;
  g_autoptr(RegisterData) data = NULL;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options_builder,
                           register_options, G_N_ELEMENTS (register_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_builder_end (&options_builder);

  session = xdp_save_restore_session_new (save_restore, invocation, options, &error);
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

  data = register_data_new (save_restore, invocation, session);
  xdp_dbus_impl_save_restore_call_register (save_restore->impl,
                                            session->id,
                                            session->app_id,
                                            g_variant_builder_end (&options_builder),
                                            save_restore->cancellable,
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
  XdpSaveRestore *save_restore = XDP_SAVE_RESTORE (object);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_save_restore_call_discarded_instance_ids (save_restore->impl,
                                                          session_id,
                                                          instance_ids,
                                                          save_restore->cancellable,
                                                          NULL, NULL);

  xdp_dbus_save_restore_complete_discarded_instance_ids (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_save_hint_response (XdpDbusSaveRestore    *object,
                           GDBusMethodInvocation *invocation,
                           const char            *session_id)
{
  XdpSaveRestore *save_restore = XDP_SAVE_RESTORE (object);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_save_restore_call_save_hint_response (save_restore->impl,
                                                      session->id,
                                                      save_restore->cancellable,
                                                      NULL, NULL);

  xdp_dbus_save_restore_complete_save_hint_response (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_save_hint (XdpDbusImplSaveRestore *impl,
              const char             *session_id,
              gpointer                data)
{
  XdpSaveRestore *save_restore = XDP_SAVE_RESTORE (data);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);
  XdpSaveRestoreSession *save_restore_session = XDP_SAVE_RESTORE_SESSION (session);

  if (save_restore_session && !save_restore_session->closed)
    xdp_dbus_save_restore_emit_save_hint (XDP_DBUS_SAVE_RESTORE (save_restore),
                                          session->id);
}

static void
on_quit (XdpDbusImplSaveRestore *impl,
         const char             *session_id,
         gpointer                data)
{
  XdpSaveRestore *save_restore = XDP_SAVE_RESTORE (data);
  g_autoptr(XdpSession) session = xdp_session_lookup (session_id);
  XdpSaveRestoreSession *save_restore_session = XDP_SAVE_RESTORE_SESSION (session);

  if (save_restore_session && !save_restore_session->closed)
    xdp_dbus_save_restore_emit_save_hint (XDP_DBUS_SAVE_RESTORE (save_restore),
                                          session->id);
}

static void
xdp_save_restore_iface_init (XdpDbusSaveRestoreIface *iface)
{
  iface->handle_register = handle_register;
  iface->handle_discarded_instance_ids = handle_discarded_instance_ids;
  iface->handle_save_hint_response = handle_save_hint_response;
}

static void
xdp_save_restore_dispose (GObject *object)
{
  XdpSaveRestore *save_restore = XDP_SAVE_RESTORE (object);

  g_cancellable_cancel (save_restore->cancellable);
  g_clear_object (&save_restore->cancellable);
  g_clear_object (&save_restore->impl);

  G_OBJECT_CLASS (xdp_save_restore_parent_class)->dispose (object);
}

static void
xdp_save_restore_init (XdpSaveRestore *save_restore)
{
}

static void
xdp_save_restore_class_init (XdpSaveRestoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_save_restore_dispose;
}

static XdpSaveRestore *
save_restore_new (XdpDbusImplSaveRestore *impl)
{
  XdpSaveRestore *save_restore;

  save_restore = g_object_new (XDP_TYPE_SAVE_RESTORE, NULL);
  save_restore->impl = g_object_ref (impl);
  save_restore->cancellable = g_cancellable_new ();

  g_signal_connect_object (save_restore->impl, "save-hint",
                           G_CALLBACK (on_save_hint),
                           save_restore,
                           G_CONNECT_DEFAULT);
  g_signal_connect_object (save_restore->impl, "quit",
                           G_CALLBACK (on_quit),
                           save_restore,
                           G_CONNECT_DEFAULT);

  xdp_dbus_save_restore_set_version (XDP_DBUS_SAVE_RESTORE (save_restore), 1);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (save_restore->impl), G_MAXINT);

  return save_restore;
}

void
init_save_restore (XdpContext *context)
{
  g_autoptr(XdpSaveRestore) save_restore = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplSaveRestore) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SAVE_RESTORE_DBUS_IMPL_IFACE);
  if (impl_config != NULL)
    return;

  impl = xdp_dbus_impl_save_restore_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    impl_config->dbus_name,
                                                    DESKTOP_DBUS_PATH,
                                                    NULL,
                                                    &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create save/restore proxy: %s", error->message);
      return;
    }

  save_restore = save_restore_new (impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&save_restore)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
