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

#if HAVE_VARLINK
#include "xdp-varlink.h"
#endif

typedef struct _Clipboard Clipboard;
typedef struct _ClipboardClass ClipboardClass;

struct _Clipboard
{
  XdpDbusClipboardSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplClipboard *impl;

#if HAVE_VARLINK
  /* Object path -> Subscriptions */
  GHashTable *subscriptions;
#endif
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
#if HAVE_VARLINK
  g_clear_pointer (&clipboard->subscriptions, g_hash_table_unref);
#endif

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

#if HAVE_VARLINK

#define VARLINK_INTERFACE "org.freedesktop.portal.Clipboard"

static const char varlink_interface_description[] =
  "# The Clipboard portal allows sessions to access the clipboard.\n"
  "#\n"
  "# This portal does not create its own sessions. Instead, it extends\n"
  "# sessions created by other portals with clipboard access. Currently, the\n"
  "# RemoteDesktop and InputCapture portals support clipboard integration.\n"
  "#\n"
  "# The clipboard uses a MIME type-based ownership and transfer model. Files\n"
  "# can be transferred through the clipboard using the\n"
  "# `application/vnd.portal.filetransfer` MIME type.\n"
  "#\n"
  "# A connection carrying a `more` call is busy until the final reply, so a\n"
  "# client uses one connection per subscription\n"
  "interface " VARLINK_INTERFACE "\n"
  "\n"
  "# Requests clipboard access for the given portal session. This request must\n"
  "# be made before the session starts. The session must be started, before\n"
  "# using any other method in this interface which take a session. Note that\n"
  "# other interfaces might place restriction on when it's possible to\n"
  "# interact with the clipboard.\n"
  "#\n"
  "# Whether clipboard access was granted is reported in the\n"
  "# clipboard_enabled result of the session's Start reply\n"
  "method RequestClipboard(session: int) -> ()\n"
  "\n"
  "# Sets the owner of the clipboard formats in mime_types to the session,\n"
  "# i.e. this session has data for the advertised clipboard formats.\n"
  "#\n"
  "# May only be called if clipboard access was given after starting the\n"
  "# session\n"
  "method SetSelection(session: int, mime_types: []string) -> ()\n"
  "\n"
  "# Transfer the clipboard content given the specified mime type to the\n"
  "# method caller via a file descriptor. The creation of the file descriptor\n"
  "# is the responsibility of the callee.\n"
  "#\n"
  "# May only be called if clipboard access was given after starting the\n"
  "# session\n"
  "method SelectionRead(session: int, mime_type: string) -> (fd_idx: int)\n"
  "\n"
  "# Answer to a SubscribeSelectionTransfer reply. Transfers the clipboard\n"
  "# content for the given serial to the method callee via a file descriptor.\n"
  "# It is the Callee that creates the file descriptor.\n"
  "#\n"
  "# May only be called if clipboard access was given after starting the\n"
  "# session\n"
  "method SelectionWrite(session: int, serial: int) -> (fd_idx: int)\n"
  "\n"
  "# Notifies that the transfer of the clipboard data has either completed\n"
  "# successfully, or failed\n"
  "method SelectionWriteDone(session: int, serial: int, success: bool) -> ()\n"
  "\n"
  "# Notifies the session that the clipboard selection has changed. mime_types\n"
  "# is a list of MIME types for which the new clipboard selection has\n"
  "# content, and session_is_owner whether the session is the owner of the\n"
  "# clipboard selection.\n"
  "#\n"
  "# Call with `more` on a dedicated connection. The final reply means the\n"
  "# session ended\n"
  "method SubscribeSelectionOwnerChanged(session: int) -> (\n"
  "  mime_types: []string,\n"
  "  session_is_owner: bool\n"
  ")\n"
  "\n"
  "# Notifies the session of a request for clipboard content of the given mime\n"
  "# type. The callee provides a serial to track the request, which any\n"
  "# SelectionWrite responses must use.\n"
  "#\n"
  "# Once the caller is done handling the request, they must call\n"
  "# SelectionWriteDone with the corresponding request's serial and whether\n"
  "# the request completed successfully. If the request is not handled, the\n"
  "# caller should respond by setting success to false.\n"
  "#\n"
  "# Call with `more` on its own connection, separate from the one used for\n"
  "# SubscribeSelectionOwnerChanged\n"
  "method SubscribeSelectionTransfer(session: int) -> (\n"
  "  mime_type: string,\n"
  "  serial: int\n"
  ")\n"
  "\n"
  "# No session with this id exists, or the caller is not authorized for it\n"
  "error NoSuchSession()\n"
  "\n"
  "# Clipboard access was not given, or the session is not in a state where\n"
  "# the clipboard may be used\n"
  "error NotAllowed()\n"
  "\n"
  "# The call must be made with `more`\n"
  "error ExpectedMore()\n"
  "\n"
  "error " XDP_VARLINK_ERROR_NOT_REGISTERED "()\n";

/* Keyed by the session object path, which is what the impl signals name */
typedef struct
{
  GPtrArray *owner_changed;
  GPtrArray *transfer;
} Subscriptions;

static void
varlink_call_free (gpointer data)
{
  varlink_call_unref (data);
}

static Subscriptions *
subscriptions_new (void)
{
  Subscriptions *subscriptions = g_new0 (Subscriptions, 1);

  subscriptions->owner_changed =
    g_ptr_array_new_with_free_func (varlink_call_free);
  subscriptions->transfer = g_ptr_array_new_with_free_func (varlink_call_free);

  return subscriptions;
}

static void
subscriptions_free (gpointer data)
{
  Subscriptions *subscriptions = data;

  g_ptr_array_unref (subscriptions->owner_changed);
  g_ptr_array_unref (subscriptions->transfer);
  g_free (subscriptions);
}

/* Replies to every parked call, dropping the ones whose connection is gone */
static void
reply_to_subscriptions (GPtrArray     *calls,
                        VarlinkObject *reply)
{
  for (guint i = calls->len; i > 0; i--)
    {
      if (xdp_varlink_call_reply (g_ptr_array_index (calls, i - 1), reply,
                                  VARLINK_REPLY_CONTINUES) ==
          -VARLINK_ERROR_CONNECTION_CLOSED)
        g_ptr_array_remove_index_fast (calls, i - 1);
    }
}

static GObject *
varlink_lookup_session (Clipboard            *clipboard,
                        XdpVarlinkConnection *connection,
                        VarlinkObject        *parameters,
                        long                 *error_out)
{
  g_autofree char *key = NULL;
  int64_t handle;

  if (varlink_object_get_int (parameters, "session", &handle) < 0)
    {
      *error_out = -1;
      return NULL;
    }

  key = g_strdup_printf ("%" G_GINT64_FORMAT, handle);

  return clipboard_session_lookup (clipboard, key,
                                   xdp_varlink_connection_get_app_info (connection));
}

static long
reply_no_such_session (VarlinkCall *call)
{
  return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSuchSession", NULL);
}

static long
reply_not_allowed (VarlinkCall *call)
{
  return varlink_call_reply_error (call, VARLINK_INTERFACE ".NotAllowed", NULL);
}

static long
handle_varlink_request_clipboard (XdpVarlinkService    *service,
                                  XdpVarlinkConnection *connection,
                                  VarlinkCall          *call,
                                  VarlinkObject        *parameters,
                                  uint64_t              flags,
                                  gpointer              user_data)
{
  Clipboard *clipboard = user_data;
  g_autoptr(GObject) session = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  long lookup_error = 0;

  session = varlink_lookup_session (clipboard, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  if (!session_supports_clipboard (session))
    return reply_no_such_session (call);

  if (!session_can_request_clipboard (session))
    return reply_not_allowed (call);

  /* Awaited so a backend refusal is reported here, rather than surfacing as
   * a session that was never granted the clipboard */
  if (!dex_await (xdp_dbus_impl_clipboard_call_request_clipboard_future (
                    clipboard->impl,
                    session_get_object_path (session),
                    g_variant_builder_end (&options_builder)),
                  &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      return reply_not_allowed (call);
    }

  session_clipboard_requested (session);

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

/* Every call below RequestClipboard needs the clipboard to be usable now */
static GObject *
lookup_usable_session (Clipboard            *clipboard,
                       XdpVarlinkConnection *connection,
                       VarlinkObject        *parameters,
                       VarlinkCall          *call,
                       long                 *result)
{
  g_autoptr(GObject) session = NULL;
  long lookup_error = 0;

  session = varlink_lookup_session (clipboard, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      *result = lookup_error < 0
                  ? varlink_call_reply_invalid_parameter (call, "session")
                  : reply_no_such_session (call);
      return NULL;
    }

  if (!session_supports_clipboard (session))
    {
      *result = reply_no_such_session (call);
      return NULL;
    }

  if (!session_is_clipboard_enabled (session) ||
      !session_can_access_clipboard (session))
    {
      *result = reply_not_allowed (call);
      return NULL;
    }

  return g_steal_pointer (&session);
}

static long
handle_varlink_set_selection (XdpVarlinkService    *service,
                              XdpVarlinkConnection *connection,
                              VarlinkCall          *call,
                              VarlinkObject        *parameters,
                              uint64_t              flags,
                              gpointer              user_data)
{
  Clipboard *clipboard = user_data;
  g_autoptr(GObject) session = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GStrvBuilder) mime_builder = NULL;
  g_auto(GStrv) mime_types = NULL;
  VarlinkArray *array;
  unsigned long n_elements;
  long result;

  session = lookup_usable_session (clipboard, connection, parameters, call,
                                   &result);
  if (!session)
    return result;

  if (varlink_object_get_array (parameters, "mime_types", &array) < 0)
    return varlink_call_reply_invalid_parameter (call, "mime_types");

  mime_builder = g_strv_builder_new ();
  n_elements = varlink_array_get_n_elements (array);

  for (unsigned long i = 0; i < n_elements; i++)
    {
      const char *mime_type;

      if (varlink_array_get_string (array, i, &mime_type) < 0)
        return varlink_call_reply_invalid_parameter (call, "mime_types");

      g_strv_builder_add (mime_builder, mime_type);
    }

  mime_types = g_strv_builder_end (mime_builder);

  g_variant_builder_add (&options_builder, "{sv}", "mime_types",
                         g_variant_new_strv ((const char * const *) mime_types, -1));

  xdp_dbus_impl_clipboard_call_set_selection (
    clipboard->impl,
    session_get_object_path (session),
    g_variant_builder_end (&options_builder),
    NULL, NULL, NULL);

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

/* The impl hands back a descriptor for both reads and writes, so it is the
 * only one in the batch */
static long
reply_with_fd (VarlinkCall *call,
               GUnixFDList *fd_list,
               GVariant    *handle)
{
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(GError) error = NULL;
  g_autofd int fd = -1;

  fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (handle), &error);
  if (fd < 0)
    {
      g_warning ("Failed to take the clipboard fd: %s", error->message);
      return reply_not_allowed (call);
    }

  if (varlink_call_push_fd (call, fd) < 0)
    return reply_not_allowed (call);

  g_steal_fd (&fd);

  varlink_object_new (&reply);
  varlink_object_set_int (reply, "fd_idx", 0);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_selection_read (XdpVarlinkService    *service,
                               XdpVarlinkConnection *connection,
                               VarlinkCall          *call,
                               VarlinkObject        *parameters,
                               uint64_t              flags,
                               gpointer              user_data)
{
  Clipboard *clipboard = user_data;
  g_autoptr(GObject) session = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;
  const char *mime_type;
  long result;

  session = lookup_usable_session (clipboard, connection, parameters, call,
                                   &result);
  if (!session)
    return result;

  if (varlink_object_get_string (parameters, "mime_type", &mime_type) < 0)
    return varlink_call_reply_invalid_parameter (call, "mime_type");

  if (!xdp_dbus_impl_clipboard_call_selection_read_sync (
        clipboard->impl,
        session_get_object_path (session),
        mime_type,
        NULL,
        &fd_handle,
        &fd_list,
        NULL,
        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      return reply_not_allowed (call);
    }

  return reply_with_fd (call, fd_list, fd_handle);
}

static long
handle_varlink_selection_write (XdpVarlinkService    *service,
                                XdpVarlinkConnection *connection,
                                VarlinkCall          *call,
                                VarlinkObject        *parameters,
                                uint64_t              flags,
                                gpointer              user_data)
{
  Clipboard *clipboard = user_data;
  g_autoptr(GObject) session = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) fd_handle = NULL;
  g_autoptr(GError) error = NULL;
  int64_t serial;
  long result;

  session = lookup_usable_session (clipboard, connection, parameters, call,
                                   &result);
  if (!session)
    return result;

  if (varlink_object_get_int (parameters, "serial", &serial) < 0)
    return varlink_call_reply_invalid_parameter (call, "serial");

  if (!xdp_dbus_impl_clipboard_call_selection_write_sync (
        clipboard->impl,
        session_get_object_path (session),
        serial,
        NULL,
        &fd_handle,
        &fd_list,
        NULL,
        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      return reply_not_allowed (call);
    }

  return reply_with_fd (call, fd_list, fd_handle);
}

static long
handle_varlink_selection_write_done (XdpVarlinkService    *service,
                                     XdpVarlinkConnection *connection,
                                     VarlinkCall          *call,
                                     VarlinkObject        *parameters,
                                     uint64_t              flags,
                                     gpointer              user_data)
{
  Clipboard *clipboard = user_data;
  g_autoptr(GObject) session = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  int64_t serial;
  bool success;
  long result;

  session = lookup_usable_session (clipboard, connection, parameters, call,
                                   &result);
  if (!session)
    return result;

  if (varlink_object_get_int (parameters, "serial", &serial) < 0)
    return varlink_call_reply_invalid_parameter (call, "serial");

  if (varlink_object_get_bool (parameters, "success", &success) < 0)
    return varlink_call_reply_invalid_parameter (call, "success");

  xdp_dbus_impl_clipboard_call_selection_write_done (
    clipboard->impl,
    session_get_object_path (session),
    serial,
    success,
    NULL, NULL, NULL);

  varlink_object_new (&reply);

  return varlink_call_reply (call, reply, 0);
}

static Subscriptions *
subscriptions_for (Clipboard  *clipboard,
                   const char *object_path,
                   gboolean    create)
{
  Subscriptions *subscriptions;

  subscriptions = g_hash_table_lookup (clipboard->subscriptions, object_path);
  if (subscriptions || !create)
    return subscriptions;

  subscriptions = subscriptions_new ();
  g_hash_table_insert (clipboard->subscriptions, g_strdup (object_path),
                       subscriptions);

  return subscriptions;
}

static long
subscribe (Clipboard            *clipboard,
           XdpVarlinkConnection *connection,
           VarlinkCall          *call,
           VarlinkObject        *parameters,
           uint64_t              flags,
           gboolean              transfer)
{
  g_autoptr(GObject) session = NULL;
  Subscriptions *subscriptions;
  long lookup_error = 0;

  if (!(flags & VARLINK_CALL_MORE))
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".ExpectedMore", NULL);

  session = varlink_lookup_session (clipboard, connection, parameters,
                                    &lookup_error);
  if (!session)
    {
      if (lookup_error < 0)
        return varlink_call_reply_invalid_parameter (call, "session");

      return reply_no_such_session (call);
    }

  if (!session_supports_clipboard (session))
    return reply_no_such_session (call);

  subscriptions = subscriptions_for (clipboard,
                                     session_get_object_path (session), TRUE);

  g_ptr_array_add (transfer ? subscriptions->transfer
                            : subscriptions->owner_changed,
                   varlink_call_ref (call));

  return 0;
}

static long
handle_varlink_subscribe_selection_owner_changed (XdpVarlinkService    *service,
                                                  XdpVarlinkConnection *connection,
                                                  VarlinkCall          *call,
                                                  VarlinkObject        *parameters,
                                                  uint64_t              flags,
                                                  gpointer              user_data)
{
  return subscribe (user_data, connection, call, parameters, flags, FALSE);
}

static long
handle_varlink_subscribe_selection_transfer (XdpVarlinkService    *service,
                                             XdpVarlinkConnection *connection,
                                             VarlinkCall          *call,
                                             VarlinkObject        *parameters,
                                             uint64_t              flags,
                                             gpointer              user_data)
{
  return subscribe (user_data, connection, call, parameters, flags, TRUE);
}

static const XdpVarlinkMethod varlink_methods[] = {
  { "RequestClipboard", handle_varlink_request_clipboard, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SetSelection", handle_varlink_set_selection, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SelectionRead", handle_varlink_selection_read, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SelectionWrite", handle_varlink_selection_write, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SelectionWriteDone", handle_varlink_selection_write_done, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SubscribeSelectionOwnerChanged", handle_varlink_subscribe_selection_owner_changed, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SubscribeSelectionTransfer", handle_varlink_subscribe_selection_transfer, XDP_VARLINK_METHOD_FLAGS_NONE },
};

gboolean
init_clipboard_varlink (XdpVarlinkService  *service,
                        XdpContext         *context,
                        GError            **error)
{
  GDBusInterfaceSkeleton *skeleton;

  skeleton = xdp_context_get_portal (context, CLIPBOARD_DBUS_IFACE);
  if (skeleton == NULL)
    return TRUE;

  return xdp_varlink_service_add_interface (service, varlink_interface_description,
                                            varlink_methods,
                                            G_N_ELEMENTS (varlink_methods),
                                            g_object_ref (skeleton),
                                            g_object_unref, error);
}

#endif /* HAVE_VARLINK */

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

  if (!session_is_clipboard_enabled (session) || session_is_closed (session))
    return;

#if HAVE_VARLINK
  {
    Subscriptions *subscriptions =
      subscriptions_for (clipboard, arg_session_handle, FALSE);

    if (subscriptions && subscriptions->transfer->len > 0)
      {
        g_autoptr(VarlinkObject) reply = NULL;

        varlink_object_new (&reply);
        varlink_object_set_string (reply, "mime_type", arg_mime_type);
        varlink_object_set_int (reply, "serial", arg_serial);

        reply_to_subscriptions (subscriptions->transfer, reply);
        return;
      }
  }
#endif

  g_dbus_connection_emit_signal (
    connection,
    session_get_sender (session),
    DESKTOP_DBUS_PATH,
    CLIPBOARD_DBUS_IFACE,
    "SelectionTransfer",
    g_variant_new ("(osu)", arg_session_handle, arg_mime_type, arg_serial),
    NULL);
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

  if (!session_is_clipboard_enabled (session) || session_is_closed (session))
    return;

#if HAVE_VARLINK
  {
    Subscriptions *subscriptions =
      subscriptions_for (clipboard, arg_session_handle, FALSE);

    if (subscriptions && subscriptions->owner_changed->len > 0)
      {
        g_autoptr(VarlinkObject) reply = NULL;
        g_autoptr(VarlinkArray) mime_types = NULL;
        g_autoptr(GVariant) mime_types_variant = NULL;
        gboolean session_is_owner = FALSE;

        varlink_array_new (&mime_types);

        mime_types_variant = g_variant_lookup_value (arg_options, "mime_types",
                                                     G_VARIANT_TYPE_STRING_ARRAY);
        if (mime_types_variant)
          {
            GVariantIter iter;
            const char *mime_type;

            g_variant_iter_init (&iter, mime_types_variant);
            while (g_variant_iter_next (&iter, "&s", &mime_type))
              varlink_array_append_string (mime_types, mime_type);
          }

        g_variant_lookup (arg_options, "session_is_owner", "b", &session_is_owner);

        varlink_object_new (&reply);
        varlink_object_set_array (reply, "mime_types", mime_types);
        varlink_object_set_bool (reply, "session_is_owner", session_is_owner);

        reply_to_subscriptions (subscriptions->owner_changed, reply);
        return;
      }
  }
#endif

  g_dbus_connection_emit_signal (
    connection,
    session_get_sender (session),
    DESKTOP_DBUS_PATH,
    CLIPBOARD_DBUS_IFACE,
    "SelectionOwnerChanged",
    g_variant_new ("(o@a{sv})", arg_session_handle, arg_options),
    NULL);
}



static Clipboard *
clipboard_new (XdpContext           *context,
               XdpDbusImplClipboard *impl)
{
  Clipboard *clipboard;

  clipboard = g_object_new (clipboard_get_type (), NULL);
  clipboard->context = context;
  clipboard->impl = g_object_ref (impl);
#if HAVE_VARLINK
  clipboard->subscriptions =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, subscriptions_free);
#endif

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
