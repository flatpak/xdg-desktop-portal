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

#include "clipboard.h"
#include "remote-desktop.h"
#include "session.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Clipboard Clipboard;
typedef struct _ClipboardClass ClipboardClass;

struct _Clipboard
{
  XdpDbusClipboardSkeleton parent_instance;
};

struct _ClipboardClass
{
  XdpDbusClipboardSkeletonClass parent_class;
};

static XdpDbusImplClipboard *impl;
static Clipboard *clipboard;

GType clipboard_get_type (void) G_GNUC_CONST;
static void clipboard_iface_init (XdpDbusClipboardIface *iface);

G_DEFINE_TYPE_WITH_CODE (Clipboard,
                         clipboard,
                         XDP_DBUS_TYPE_CLIPBOARD_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_CLIPBOARD,
                                                clipboard_iface_init))

static XdpOptionKey clipboard_set_selection_options[] = {
  { "mime_types", G_VARIANT_TYPE_STRING_ARRAY, NULL },
};

static gboolean
handle_request_clipboard (XdpDbusClipboard *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_session_handle,
                          GVariant *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  RemoteDesktopSession *remote_desktop_session;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!IS_REMOTE_DESKTOP_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  if (!remote_desktop_session_can_request_clipboard (remote_desktop_session))
    {
      g_dbus_method_invocation_return_error (
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Invalid state");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_request_clipboard (
    impl, session->id, arg_options, NULL, NULL, NULL);

  xdp_dbus_clipboard_complete_request_clipboard (object, invocation);
  remote_desktop_session_clipboard_requested (remote_desktop_session);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_selection (XdpDbusClipboard *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_session_handle,
                      GVariant *arg_options)
{
  Call *call = call_from_invocation (invocation);
  Session *session;
  GVariantBuilder options_builder;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!IS_REMOTE_DESKTOP_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  else if (!remote_desktop_session_is_clipboard_enabled (
             REMOTE_DESKTOP_SESSION (session)))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options,
                           &options_builder,
                           clipboard_set_selection_options,
                           G_N_ELEMENTS (clipboard_set_selection_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_dbus_impl_clipboard_call_set_selection (
    impl, arg_session_handle, options, NULL, NULL, NULL);

  xdp_dbus_clipboard_complete_set_selection (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
selection_write_done (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&user_data);
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;
  int fd;
  int fd_id;
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
      fd_id = g_variant_get_handle (fd_handle);
      fd = g_unix_fd_list_get (fd_list, fd_id, &error);

      out_fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);

      close (fd);

      if (out_fd_id == -1)
        {
          g_dbus_method_invocation_return_error (
            invocation,
            XDG_DESKTOP_PORTAL_ERROR,
            XDG_DESKTOP_PORTAL_ERROR_FAILED,
            "Failed to append fd: %s",
            error->message);
        }
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
  Call *call = call_from_invocation (invocation);
  Session *session;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!IS_REMOTE_DESKTOP_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  else if (!remote_desktop_session_is_clipboard_enabled (
             REMOTE_DESKTOP_SESSION (session)))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_selection_write (impl,
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
  Call *call = call_from_invocation (invocation);
  Session *session;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!IS_REMOTE_DESKTOP_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  else if (!remote_desktop_session_is_clipboard_enabled (
             REMOTE_DESKTOP_SESSION (session)))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_selection_write_done (
    impl, arg_session_handle, arg_serial, arg_success, NULL, NULL, NULL);

  xdp_dbus_clipboard_complete_selection_write_done (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
selection_read_done (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&user_data);
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;

  int fd;
  int fd_id;
  int out_fd_id;

  if (!xdp_dbus_impl_clipboard_call_selection_read_finish (
        impl, &fd_handle, &fd_list, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
    }

  fd_id = g_variant_get_handle (fd_handle);
  fd = g_unix_fd_list_get (fd_list, fd_id, &error);

  out_fd_list = g_unix_fd_list_new ();
  out_fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);

  if (out_fd_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to append fd: %s",
                                             error->message);
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
  Call *call = call_from_invocation (invocation);
  Session *session;

  session = acquire_session_from_call (arg_session_handle, call);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  if (!IS_REMOTE_DESKTOP_SESSION (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  else if (!remote_desktop_session_is_clipboard_enabled (
              REMOTE_DESKTOP_SESSION (session)))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Clipboard not enabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_impl_clipboard_call_selection_read (impl,
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
clipboard_init (Clipboard *clipboard)
{
  xdp_dbus_clipboard_set_version (XDP_DBUS_CLIPBOARD (clipboard), 1);
}

static void
clipboard_class_init (ClipboardClass *klass)
{
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
  Session *session;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Cannot find session");
      return;
    }

  SESSION_AUTOLOCK_UNREF (session);

  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  if (remote_desktop_session &&
      remote_desktop_session_is_clipboard_enabled (remote_desktop_session) &&
      !session->closed)
    {
      g_dbus_connection_emit_signal (
        connection,
        session->sender,
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Clipboard",
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
  Session *session;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Cannot find session");
      return;
    }

  SESSION_AUTOLOCK_UNREF (session);

  RemoteDesktopSession *remote_desktop_session = REMOTE_DESKTOP_SESSION (session);

  if (remote_desktop_session &&
      remote_desktop_session_is_clipboard_enabled (remote_desktop_session) &&
      !session->closed)
    {
      g_dbus_connection_emit_signal (
        connection,
        session->sender,
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Clipboard",
        "SelectionOwnerChanged",
        g_variant_new ("(o@a{sv})", arg_session_handle, arg_options),
        NULL);
    }
}

GDBusInterfaceSkeleton *
clipboard_create (GDBusConnection *connection,
                  const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_clipboard_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 dbus_name,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create clipboard: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  clipboard = g_object_new (clipboard_get_type (), NULL);

  g_signal_connect (
    impl, "selection-transfer", G_CALLBACK (selection_transfer_cb), clipboard);

  g_signal_connect (impl,
                    "selection-owner-changed",
                    G_CALLBACK (selection_owner_changed_cb),
                    clipboard);

  return G_DBUS_INTERFACE_SKELETON (clipboard);
}
