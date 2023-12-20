/*
 * Copyright © 2017 Red Hat, Inc
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
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
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

#include "email.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Email Email;
typedef struct _EmailClass EmailClass;

struct _Email
{
  XdpDbusEmailSkeleton parent_instance;
};

struct _EmailClass
{
  XdpDbusEmailSkeletonClass parent_class;
};

static XdpDbusImplEmail *impl;
static Email *email;

GType email_get_type (void) G_GNUC_CONST;
static void email_iface_init (XdpDbusEmailIface *iface);

G_DEFINE_TYPE_WITH_CODE (Email, email, XDP_DBUS_TYPE_EMAIL_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_EMAIL,
                                                email_iface_init));

static void
send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  Request *request = task_data;
  guint response;
  GVariantBuilder new_results;

  g_variant_builder_init (&new_results, G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));

  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&new_results));
      request_unexport (request);
    }
}

static void
compose_email_done (GObject *source,
                    GAsyncResult *result,
                    gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_email_call_compose_email_finish (XDP_DBUS_IMPL_EMAIL (source),
                                                      &response,
                                                      &results,
                                                      result,
                                                      &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
is_valid_email (const char *string)
{
  // Regex proposed by the W3C at https://html.spec.whatwg.org/multipage/input.html#valid-e-mail-address
  return g_regex_match_simple ("^[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+"
                               "@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?"
                               "(?:\\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$",
                               string, 0, 0);
}

static gboolean
validate_email_address (const char *key,
                        GVariant *value,
                        GVariant *options,
                        GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (!is_valid_email (string))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "'%s' does not look like an email address", string);
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_email_addresses (const char *key,
                          GVariant *value,
                          GVariant *options,
                          GError **error)
{
  g_autofree const char *const *strings = g_variant_get_strv (value, NULL);
  int i;

  for (i = 0; strings[i]; i++)
    {
      if (!is_valid_email (strings[i]))
        {
          g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "'%s' does not look like an email address", strings[i]);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
validate_email_subject (const char *key,
                        GVariant *value,
                        GVariant *options,
                        GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (strchr (string, '\n'))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting multi-line subjects");
      return FALSE;
    } 

  if (g_utf8_strlen (string, -1) > 200)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
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

static gboolean
handle_compose_email (XdpDbusEmail *object,
                      GDBusMethodInvocation *invocation,
                      GUnixFDList *fd_list,
                      const gchar *arg_parent_window,
                      GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariantBuilder options;
  g_autoptr(GVariant) attachment_fds = NULL;

  g_debug ("Handling ComposeEmail");

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                       g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                       request->id,
                                                       NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);

  attachment_fds = g_variant_lookup_value (arg_options, "attachment_fds", G_VARIANT_TYPE ("ah"));
  if (attachment_fds)
    {
      GVariantBuilder attachments;
      int i;

      g_variant_builder_init (&attachments, G_VARIANT_TYPE_STRING_ARRAY);
      for (i = 0; i < g_variant_n_children (attachment_fds); i++)
        {
          g_autofree char *path = NULL;
          int fd_id;
          int fd;

          g_variant_get_child (attachment_fds, i, "h", &fd_id);
          fd = g_unix_fd_list_get (fd_list, fd_id, &error);
          if (fd == -1)
            {
              g_dbus_method_invocation_return_gerror (invocation, error);
              return G_DBUS_METHOD_INVOCATION_HANDLED;
            }

          path = xdp_app_info_get_path_for_fd (request->app_info, fd, 0, NULL, NULL, &error);

          if (path == NULL)
            {
              g_debug ("Invalid attachment fd passed: %s", error->message);

              /* Don't leak any info about real file path existence, etc */
              g_dbus_method_invocation_return_error (invocation,
                                                     XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                     "Invalid attachment fd passed");
              return G_DBUS_METHOD_INVOCATION_HANDLED;
            }

          g_variant_builder_add (&attachments, "s", path);
        }

      g_variant_builder_add (&options, "{sv}", "attachments", g_variant_builder_end (&attachments));
    }

  if (!xdp_filter_options (arg_options, &options,
                           compose_email_options, G_N_ELEMENTS (compose_email_options),
                           &error))
    {
      g_debug ("Returning an error from option filtering");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_email_complete_compose_email (object, invocation, NULL, request->id);

  xdp_dbus_impl_email_call_compose_email (impl,
                                          request->id,
                                          app_id,
                                          arg_parent_window,
                                          g_variant_builder_end (&options),
                                          NULL,
                                          compose_email_done,
                                          g_object_ref (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
email_iface_init (XdpDbusEmailIface *iface)
{
  iface->handle_compose_email = handle_compose_email;
}

static void
email_init (Email *email)
{
  xdp_dbus_email_set_version (XDP_DBUS_EMAIL (email), 4);
}

static void
email_class_init (EmailClass *klass)
{
}

GDBusInterfaceSkeleton *
email_create (GDBusConnection *connection,
              const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_email_proxy_new_sync (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             dbus_name,
                                             DESKTOP_PORTAL_OBJECT_PATH,
                                             NULL,
                                             &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create email proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  email = g_object_new (email_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (email);
}
