/*
 * Copyright 2022 Google LLC
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

#include <gio/gunixfdlist.h>
#include <stdint.h>

#include "input-capture.h"
#include "remote-desktop.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

#include "clipboard.h"

typedef struct _Clipboard Clipboard;
typedef struct _ClipboardClass ClipboardClass;

struct _Clipboard
{
  XdpDbusClipboardSkeleton parent_instance;

  XdpDbusImplClipboard *impl;
};

struct _ClipboardClass
{
  XdpDbusClipboardSkeletonClass parent_class;
};

GType clipboard_get_type (void) G_GNUC_CONST;
static void clipboard_iface_init (XdpDbusClipboardIface *iface);

G_DEFINE_TYPE_WITH_CODE (Clipboard,
                         clipboard,
                         XDP_DBUS_TYPE_CLIPBOARD_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_CLIPBOARD,
                                                clipboard_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Clipboard, g_object_unref)

static gboolean
session_supports_clipboard (XdpSession *session)
{
  return IS_REMOTE_DESKTOP_SESSION (session) ||
         IS_INPUT_CAPTURE_SESSION (session);
}

static gboolean
session_can_request_clipboard (XdpSession *session)
{
  if (IS_REMOTE_DESKTOP_SESSION (session))
    return remote_desktop_session_can_request_clipboard (REMOTE_DESKTOP_SESSION (session));
  else if (IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_can_request_clipboard (INPUT_CAPTURE_SESSION (session));
  else
    g_assert_not_reached ();
}

static void
session_clipboard_requested (XdpSession *session)
{
  if (IS_REMOTE_DESKTOP_SESSION (session))
    remote_desktop_session_clipboard_requested (REMOTE_DESKTOP_SESSION (session));
  else if (IS_INPUT_CAPTURE_SESSION (session))
    input_capture_session_clipboard_requested (INPUT_CAPTURE_SESSION (session));
  else
    g_assert_not_reached ();
}

static gboolean
session_is_clipboard_enabled (XdpSession *session)
{
  if (IS_REMOTE_DESKTOP_SESSION (session))
    return remote_desktop_session_is_clipboard_enabled (REMOTE_DESKTOP_SESSION (session));
  else if (IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_is_clipboard_enabled (INPUT_CAPTURE_SESSION (session));
  else
    g_assert_not_reached ();
}

static XdpOptionKey clipboard_set_selection_options[] = {
  { "mime_types", G_VARIANT_TYPE_STRING_ARRAY, NULL },
};

static gboolean
handle_request_clipboard (XdpDbusClipboard *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_session_handle,
                          GVariant *arg_options)
{
  Clipboard *clipboard = (Clipboard *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  XdpSession *session;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!session_supports_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!session_can_request_clipboard (session))
    {
      g_dbus_method_invocation_return_error (
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Invalid state");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_request_clipboard (clipboard->impl,
                                                  session->id,
                                                  arg_options,
                                                  NULL, NULL, NULL);

  session_clipboard_requested (session);

  xdp_dbus_clipboard_complete_request_clipboard (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_selection (XdpDbusClipboard *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_session_handle,
                      GVariant *arg_options)
{
  Clipboard *clipboard = (Clipboard *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  XdpSession *session;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!session_supports_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!session_is_clipboard_enabled (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options,
                           &options_builder,
                           clipboard_set_selection_options,
                           G_N_ELEMENTS (clipboard_set_selection_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_clipboard_call_set_selection (clipboard->impl,
                                              arg_session_handle,
                                              options,
                                              NULL, NULL, NULL);

  xdp_dbus_clipboard_complete_set_selection (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
selection_write_done (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  XdpDbusImplClipboard *impl = (XdpDbusImplClipboard *) source_object;
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&user_data);
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;
  int out_fd_id = -1;

  if (!xdp_dbus_impl_clipboard_call_selection_write_finish (
        impl, &fd_handle, &fd_list, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  out_fd_list = g_unix_fd_list_new ();

  if (fd_handle)
    {
      int fd_id = g_variant_get_handle (fd_handle);

      if (fd_id < g_unix_fd_list_get_length (fd_list))
        {
          g_autofd int fd = -1;

          fd = g_unix_fd_list_get (fd_list, fd_id, &error);

          if (fd >= 0)
            out_fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
        }
      else
        {
          g_set_error_literal (&error, XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Bad file descriptor index");
        }
    }

  if (out_fd_id == -1)
    {
      g_dbus_method_invocation_return_error (
        invocation,
        XDG_DESKTOP_PORTAL_ERROR,
        XDG_DESKTOP_PORTAL_ERROR_FAILED,
        "Failed to append fd: %s",
        error->message);
      return;
    }

  xdp_dbus_clipboard_complete_selection_write (
    NULL,
    invocation,
    out_fd_list,
    g_variant_new_handle (out_fd_id));
}

static gboolean
handle_selection_write (XdpDbusClipboard *object,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList *in_fd_list,
                        const char *arg_session_handle,
                        guint arg_serial)
{
  Clipboard *clipboard = (Clipboard *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  XdpSession *session;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!session_supports_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!session_is_clipboard_enabled (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_selection_write (clipboard->impl,
                                                arg_session_handle,
                                                arg_serial,
                                                NULL,
                                                NULL,
                                                selection_write_done,
                                                g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_selection_write_done (XdpDbusClipboard *object,
                             GDBusMethodInvocation *invocation,
                             const char *arg_session_handle,
                             guint arg_serial,
                             gboolean arg_success)
{
  Clipboard *clipboard = (Clipboard *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  XdpSession *session;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!session_supports_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!session_is_clipboard_enabled (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_selection_write_done (clipboard->impl,
                                                     arg_session_handle,
                                                     arg_serial,
                                                     arg_success,
                                                     NULL, NULL, NULL);

  xdp_dbus_clipboard_complete_selection_write_done (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
selection_read_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
  XdpDbusImplClipboard *impl = (XdpDbusImplClipboard *) source_object;
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&user_data);
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;
  int out_fd_id = -1;

  if (!xdp_dbus_impl_clipboard_call_selection_read_finish (
        impl, &fd_handle, &fd_list, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  out_fd_list = g_unix_fd_list_new ();

  if (fd_handle)
    {
      int fd_id = g_variant_get_handle (fd_handle);

      if (fd_id < g_unix_fd_list_get_length (fd_list))
        {
          g_autofd int fd = -1;

          fd = g_unix_fd_list_get (fd_list, fd_id, &error);

          if (fd >= 0)
            out_fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
        }
      else
        {
          g_set_error_literal (&error, XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Bad file descriptor index");
        }
    }

  if (out_fd_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to append fd: %s",
                                             error->message);
      return;
    }

  xdp_dbus_clipboard_complete_selection_read (
    NULL, invocation, out_fd_list, g_variant_new_handle (out_fd_id));
}

static gboolean
handle_selection_read (XdpDbusClipboard *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList *in_fd_list,
                       const char *arg_session_handle,
                       const char *arg_mime_type)
{
  Clipboard *clipboard = (Clipboard *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info  (invocation);
  XdpSession *session;

  session = xdp_session_from_app_info (arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!session_supports_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!session_is_clipboard_enabled (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_selection_read (clipboard->impl,
                                               arg_session_handle,
                                               arg_mime_type,
                                               NULL,
                                               NULL,
                                               selection_read_done,
                                               g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
clipboard_iface_init (XdpDbusClipboardIface *iface)
{
  iface->handle_request_clipboard = handle_request_clipboard;

  iface->handle_selection_read = handle_selection_read;
  iface->handle_selection_write = handle_selection_write;
  iface->handle_set_selection = handle_set_selection;
  iface->handle_selection_write_done = handle_selection_write_done;
}

static void
clipboard_dispose (GObject *object)
{
  Clipboard *clipboard = (Clipboard *) object;

  g_clear_object (&clipboard->impl);

  G_OBJECT_CLASS (clipboard_parent_class)->dispose (object);
}

static void
clipboard_init (Clipboard *clipboard)
{
}

static void
clipboard_class_init (ClipboardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = clipboard_dispose;
}

static void
selection_transfer_cb (XdpDbusImplClipboard *impl,
                       const char *arg_session_handle,
                       const char *arg_mime_type,
                       guint arg_serial,
                       gpointer data)
{
  GDBusConnection *connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  XdpSession *session;

  session = xdp_session_lookup (arg_session_handle);
  if (!session)
    {
      g_warning ("Cannot find session");
      return;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (session_is_clipboard_enabled (session) &&
      !session->closed)
    {
      g_dbus_connection_emit_signal (
        connection,
        session->sender,
        DESKTOP_DBUS_PATH,
        CLIPBOARD_DBUS_IFACE,
        "SelectionTransfer",
        g_variant_new ("(osu)", arg_session_handle, arg_mime_type, arg_serial),
        NULL);
    }
}

static void
selection_owner_changed_cb (XdpDbusImplClipboard *impl,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            gpointer data)
{
  GDBusConnection *connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  XdpSession *session;

  session = xdp_session_lookup (arg_session_handle);
  if (!session)
    {
      g_warning ("Cannot find session");
      return;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (session_is_clipboard_enabled (session) &&
      !session->closed)
    {
      g_dbus_connection_emit_signal (
        connection,
        session->sender,
        DESKTOP_DBUS_PATH,
        CLIPBOARD_DBUS_IFACE,
        "SelectionOwnerChanged",
        g_variant_new ("(o@a{sv})", arg_session_handle, arg_options),
        NULL);
    }
}

static Clipboard *
clipboard_new (XdpDbusImplClipboard *impl)
{
  Clipboard *clipboard;

  clipboard = g_object_new (clipboard_get_type (), NULL);
  clipboard->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (clipboard->impl), G_MAXINT);

  xdp_dbus_clipboard_set_version (XDP_DBUS_CLIPBOARD (clipboard), 1);

  g_signal_connect_object (clipboard->impl, "selection-transfer",
                           G_CALLBACK (selection_transfer_cb),
                           impl,
                           G_CONNECT_DEFAULT);

  g_signal_connect_object (clipboard->impl, "selection-owner-changed",
                           G_CALLBACK (selection_owner_changed_cb),
                           impl,
                           G_CONNECT_DEFAULT);

  return clipboard;
}

void
init_clipboard (XdpContext *context)
{
  g_autoptr(Clipboard) clipboard = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplClipboard) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, CLIPBOARD_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_clipboard_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 impl_config->dbus_name,
                                                 DESKTOP_DBUS_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create clipboard: %s", error->message);
      return;
    }

  clipboard = clipboard_new (impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&clipboard)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
