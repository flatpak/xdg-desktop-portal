/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "clipboard.h"

#include <stdint.h>

#include <gio/gunixfdlist.h>

#include "input-capture.h"
#include "remote-desktop.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

typedef struct _Clipboard Clipboard;
typedef struct _ClipboardClass ClipboardClass;

struct _Clipboard
{
  XdpDbusClipboardSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplClipboard *impl;
};

struct _ClipboardClass
{
  XdpDbusClipboardSkeletonClass parent_class;
};

GType clipboard_get_type (void);
static void clipboard_iface_init (XdpDbusClipboardIface *iface);

G_DEFINE_TYPE_WITH_CODE (Clipboard,
                         clipboard,
                         XDP_DBUS_TYPE_CLIPBOARD_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_CLIPBOARD,
                                                clipboard_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Clipboard, g_object_unref)

/* Input capture and remote desktop sessions share no base, so a clipboard
 * capable session is only known to be a GObject here */
static GObject *
clipboard_session_lookup (Clipboard  *clipboard,
                          const char *session_handle,
                          XdpAppInfo *app_info)
{
  XdpInputCaptureSession *input_capture_session;
  XdpSession *session;

  input_capture_session = input_capture_lookup_session (clipboard->context,
                                                        session_handle,
                                                        app_info);
  if (input_capture_session)
    return G_OBJECT (input_capture_session);

  session = app_info ? xdp_session_from_app_info (session_handle, app_info)
                     : xdp_session_lookup (session_handle);

  return session ? G_OBJECT (session) : NULL;
}

static gboolean
session_supports_clipboard (GObject *session)
{
  return XDP_IS_INPUT_CAPTURE_SESSION (session) ||
         IS_REMOTE_DESKTOP_SESSION (session);
}

static gboolean
session_can_request_clipboard (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_can_request_clipboard (XDP_INPUT_CAPTURE_SESSION (session));

  return remote_desktop_session_can_request_clipboard (REMOTE_DESKTOP_SESSION (session));
}

static void
session_clipboard_requested (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    input_capture_session_clipboard_requested (XDP_INPUT_CAPTURE_SESSION (session));
  else
    remote_desktop_session_clipboard_requested (REMOTE_DESKTOP_SESSION (session));
}

static gboolean
session_is_clipboard_enabled (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_is_clipboard_enabled (XDP_INPUT_CAPTURE_SESSION (session));

  return remote_desktop_session_is_clipboard_enabled (REMOTE_DESKTOP_SESSION (session));
}

static gboolean
session_can_access_clipboard (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_can_access_clipboard (XDP_INPUT_CAPTURE_SESSION (session));

  return remote_desktop_session_can_access_clipboard (REMOTE_DESKTOP_SESSION (session));
}

static const char *
session_get_object_path (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_get_object_path (XDP_INPUT_CAPTURE_SESSION (session));

  return XDP_SESSION (session)->id;
}

static const char *
session_get_sender (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    {
      XdpAppInfo *app_info =
        input_capture_session_get_app_info (XDP_INPUT_CAPTURE_SESSION (session));

      return xdp_app_info_get_sender (app_info);
    }

  return XDP_SESSION (session)->sender;
}

static gboolean
session_is_closed (GObject *session)
{
  if (XDP_IS_INPUT_CAPTURE_SESSION (session))
    return input_capture_session_is_closed (XDP_INPUT_CAPTURE_SESSION (session));

  return XDP_SESSION (session)->closed;
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
  g_autoptr(GObject) session = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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
                                                  session_get_object_path (session),
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
  g_autoptr(GObject) session = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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

  if (!session_can_access_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Session cannot access clipboard at the moment");
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

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Internal error");
      return;
    }

  out_fd_list = g_unix_fd_list_new ();
  if (!xdp_copy_fd_to_lists (fd_list, out_fd_list,
                             g_variant_get_handle (fd_handle),
                             &out_fd_id,
                             &error))
    {
      g_warning ("Passing a fd from impl to frontend failed: %s",
                 error->message);

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Internal error");
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
  g_autoptr(GObject) session = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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
  g_autoptr(GObject) session = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Internal error");
      return;
    }

  out_fd_list = g_unix_fd_list_new ();
  if (!xdp_copy_fd_to_lists (fd_list, out_fd_list,
                             g_variant_get_handle (fd_handle),
                             &out_fd_id,
                             &error))
    {
      g_warning ("Passing a fd from impl to frontend failed: %s",
                 error->message);

      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Internal error");
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
  g_autoptr(GObject) session = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, app_info);
  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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

  if (!session_can_access_clipboard (session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Session cannot access clipboard at the moment");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (strlen (arg_mime_type) >= 1024 * 4)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Mime type exceeds 4kb");
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
  Clipboard *clipboard = data;
  g_autoptr(GObject) session = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, NULL);
  if (!session || !session_supports_clipboard (session))
    {
      g_warning ("Cannot find session");
      return;
    }

  if (session_is_clipboard_enabled (session) &&
      !session_is_closed (session))
    {
      g_dbus_connection_emit_signal (
        connection,
        session_get_sender (session),
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
  Clipboard *clipboard = data;
  g_autoptr(GObject) session = NULL;

  session = clipboard_session_lookup (clipboard, arg_session_handle, NULL);
  if (!session || !session_supports_clipboard (session))
    {
      g_warning ("Cannot find session");
      return;
    }

  if (session_is_clipboard_enabled (session) &&
      !session_is_closed (session))
    {
      g_dbus_connection_emit_signal (
        connection,
        session_get_sender (session),
        DESKTOP_DBUS_PATH,
        CLIPBOARD_DBUS_IFACE,
        "SelectionOwnerChanged",
        g_variant_new ("(o@a{sv})", arg_session_handle, arg_options),
        NULL);
    }
}

static Clipboard *
clipboard_new (XdpContext           *context,
               XdpDbusImplClipboard *impl)
{
  Clipboard *clipboard;

  clipboard = g_object_new (clipboard_get_type (), NULL);
  clipboard->context = context;
  clipboard->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (clipboard->impl), G_MAXINT);

  xdp_dbus_clipboard_set_version (XDP_DBUS_CLIPBOARD (clipboard), 1);

  g_signal_connect_object (clipboard->impl, "selection-transfer",
                           G_CALLBACK (selection_transfer_cb),
                           clipboard,
                           G_CONNECT_DEFAULT);

  g_signal_connect_object (clipboard->impl, "selection-owner-changed",
                           G_CALLBACK (selection_owner_changed_cb),
                           clipboard,
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

  clipboard = clipboard_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&clipboard)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
}
