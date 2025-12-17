/*
 * Copyright © 2025 Red Hat, Inc
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

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "xdp-context.h"
#include "xdp-documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-request-future.h"
#include "xdp-utils.h"

#include "email.h"

struct _XdpEmail
{
  XdpDbusEmailSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplEmail *impl;
};

#define XDP_TYPE_EMAIL (xdp_email_get_type ())
G_DECLARE_FINAL_TYPE (XdpEmail,
                      xdp_email,
                      XDP, EMAIL,
                      XdpDbusEmailSkeleton)

static void xdp_email_iface_init (XdpDbusEmailIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpEmail,
                               xdp_email,
                               XDP_DBUS_TYPE_EMAIL_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_EMAIL,
                                                      xdp_email_iface_init));

static gboolean
is_valid_email (const char *string)
{
  /* Regex proposed by the W3C at
   * https://html.spec.whatwg.org/multipage/input.html#valid-e-mail-address */
  return g_regex_match_simple ("^[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+"
                               "@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?"
                               "(?:\\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$",
                               string, 0, 0);
}

static gboolean
validate_email_address (const char  *key,
                        GVariant    *value,
                        GVariant    *options,
                        gpointer     user_data,
                        GError     **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (!is_valid_email (string))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "'%s' does not look like an email address", string);
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_email_addresses (const char  *key,
                          GVariant    *value,
                          GVariant    *options,
                          gpointer     user_data,
                          GError     **error)
{
  g_autofree const char *const *strings = g_variant_get_strv (value, NULL);

  for (size_t i = 0; strings[i]; i++)
    {
      if (!is_valid_email (strings[i]))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "'%s' does not look like an email address",
                       strings[i]);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
validate_email_subject (const char  *key,
                        GVariant    *value,
                        GVariant    *options,
                        gpointer     user_data,
                        GError     **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (strchr (string, '\n'))
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "Not accepting multi-line subjects");
      return FALSE;
    }

  if (g_utf8_strlen (string, -1) > 200)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "Not accepting extremely long subjects");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey compose_email_options[] = {
  { "address", G_VARIANT_TYPE_STRING, validate_email_address },
  { "addresses", G_VARIANT_TYPE_STRING_ARRAY, validate_email_addresses },
  { "cc", G_VARIANT_TYPE_STRING_ARRAY, validate_email_addresses },
  { "bcc", G_VARIANT_TYPE_STRING_ARRAY, validate_email_addresses },
  { "subject", G_VARIANT_TYPE_STRING, validate_email_subject },
  { "body", G_VARIANT_TYPE_STRING, NULL },
  { "activation_token", G_VARIANT_TYPE_STRING, NULL },
};

static GVariant *
compose_email_validate_options (XdpAppInfo   *app_info,
                                GUnixFDList  *fd_list,
                                GVariant     *arg_options,
                                GError      **error)
{
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_auto(GVariantBuilder) attachments =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_STRING_ARRAY);
  g_autoptr(GVariant) attachment_fds = NULL;
  size_t n_attachments = 0;

  attachment_fds = g_variant_lookup_value (arg_options,
                                           "attachment_fds",
                                           G_VARIANT_TYPE ("ah"));
  if (attachment_fds)
    n_attachments = g_variant_n_children (attachment_fds);

  for (size_t i = 0; i < n_attachments; i++)
    {
      g_autofree char *path = NULL;
      int fd_id;
      g_autofd int fd = -1;

      g_variant_get_child (attachment_fds, i, "h", &fd_id);
      if (fd_id >= g_unix_fd_list_get_length (fd_list))
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Bad file descriptor index");
          return NULL;
        }

      fd = g_unix_fd_list_get (fd_list, fd_id, error);
      if (fd == -1)
        return NULL;

      path = xdp_app_info_get_path_for_fd (app_info, fd, 0, NULL, NULL, error);

      if (path == NULL)
        {
          /* Don't leak any info about real file path existence, etc */
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "Invalid attachment fd passed");
          return NULL;
        }

      g_variant_builder_add (&attachments, "s", path);
    }

  g_variant_builder_add (&options, "{sv}",
                         "attachments",
                         g_variant_builder_end (&attachments));

  if (!xdp_filter_options (arg_options,
                           &options,
                           compose_email_options,
                           G_N_ELEMENTS (compose_email_options),
                           NULL,
                           error))
    return FALSE;

  return g_variant_ref_sink (g_variant_builder_end (&options));
}

static gboolean
handle_compose_email (XdpDbusEmail          *object,
                      GDBusMethodInvocation *invocation,
                      GUnixFDList           *fd_list,
                      const gchar           *arg_parent_window,
                      GVariant              *arg_options)
{
  XdpEmail *email = XDP_EMAIL (object);
  g_autoptr(XdpRequestFuture) request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  options = compose_email_validate_options (app_info,
                                            fd_list,
                                            arg_options,
                                            &error);
  if (!options)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = dex_await_object (xdp_request_future_new (email->context,
                                                      app_info,
                                                      G_DBUS_INTERFACE_SKELETON (object),
                                                      G_DBUS_PROXY (email->impl),
                                                      arg_options),
                              &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_email_complete_compose_email (object,
                                         g_steal_pointer (&invocation),
                                         NULL,
                                         xdp_request_future_get_object_path (request));

  {
    g_autoptr(XdpDbusImplEmailComposeEmailResult) result = NULL;
    XdgDesktopPortalResponseEnum response;

    result = dex_await_boxed (xdp_dbus_impl_email_call_compose_email_future (
        email->impl,
        xdp_request_future_get_object_path (request),
        xdp_app_info_get_id (app_info),
        arg_parent_window,
        options),
      &error);

    if (result)
      {
        response = result->response;
      }
    else
      {
        g_dbus_error_strip_remote_error (error);
        g_warning ("Backend call failed: %s", error->message);

        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }

    xdp_request_future_emit_response (request, response, NULL);
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_email_iface_init (XdpDbusEmailIface *iface)
{
  iface->handle_compose_email = handle_compose_email;
}

static void
xdp_email_dispose (GObject *object)
{
  XdpEmail *email = XDP_EMAIL (object);

  g_clear_object (&email->impl);

  G_OBJECT_CLASS (xdp_email_parent_class)->dispose (object);
}

static void
xdp_email_init (XdpEmail *email)
{
}

static void
xdp_email_class_init (XdpEmailClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_email_dispose;
}

static XdpEmail *
xdp_email_new (XdpContext       *context,
               XdpDbusImplEmail *impl)
{
  XdpEmail *email;

  email = g_object_new (XDP_TYPE_EMAIL, NULL);
  email->context = context; // FIXME there might be problems with the context lifetime
  email->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (email->impl), G_MAXINT);

  xdp_dbus_email_set_version (XDP_DBUS_EMAIL (email), 4);

  return email;
}

DexFuture *
init_email (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpEmail) email = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplEmail) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, EMAIL_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_email_proxy_new_future (connection,
                                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                                 impl_config->dbus_name,
                                                                 DESKTOP_DBUS_PATH),
                           &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create email proxy: %s", error->message);
      return dex_future_new_false ();
    }

  email = xdp_email_new (context, impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&email)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  // FIXME: cancellation: must call dex_dbus_interface_skeleton_cancel at some point
  // just decreasing the ref-count doesn't stop anything because it iternally holds
  // a ref. So really, this should call g_dbus_interface_skeleton_unexport and
  // dex_dbus_interface_skeleton_cancel.
  // (should dex_dbus_interface_skeleton_cancel be part of unexport?)

  return dex_future_new_true ();
}
