/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "secret.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-request.h"
#include "xdp-types.h"
#include "xdp-request-dex.h"
#include "xdp-utils.h"

#if HAVE_VARLINK
#include "xdp-varlink.h"
#endif

typedef struct _Secret Secret;
typedef struct _SecretClass SecretClass;

struct _Secret
{
  XdpDbusSecretSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplSecret *impl;
};

struct _SecretClass
{
  XdpDbusSecretSkeletonClass parent_class;
};

GType secret_get_type (void);
static void secret_iface_init (XdpDbusSecretIface *iface);

G_DEFINE_TYPE_WITH_CODE (Secret, secret, XDP_DBUS_TYPE_SECRET_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SECRET,
                                                secret_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Secret, g_object_unref)

static XdpOptionKey retrieve_secret_options[] = {
  { "token", G_VARIANT_TYPE_STRING, NULL },
};

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  XdpRequest *request = task_data;
  guint response;
  g_auto(GVariantBuilder) new_results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));

  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&new_results));
      xdp_request_unexport (request);
    }
}

static void
retrieve_secret_done (GObject *source,
		      GAsyncResult *result,
		      gpointer data)
{
  g_autoptr(XdpRequest) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_secret_call_retrieve_secret_finish (XDP_DBUS_IMPL_SECRET (source),
                                                         &response,
                                                         &results,
                                                         NULL,
                                                         result,
                                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_source_tag (task, retrieve_secret_done);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
handle_retrieve_secret (XdpDbusSecret         *object,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList           *fd_list,
                        GVariant              *arg_fd,
                        GVariant              *arg_options)
{
  Secret *secret = (Secret *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (secret->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (secret->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_filter_options (arg_options, &options,
                           retrieve_secret_options, G_N_ELEMENTS (retrieve_secret_options),
                           NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_secret_complete_retrieve_secret (object, invocation, NULL, request->id);

  xdp_dbus_impl_secret_call_retrieve_secret (secret->impl,
                                             request->id,
                                             app_id,
                                             arg_fd,
                                             g_variant_builder_end (&options),
                                             fd_list,
                                             NULL,
                                             retrieve_secret_done,
                                             g_object_ref (request));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
secret_iface_init (XdpDbusSecretIface *iface)
{
  iface->handle_retrieve_secret = handle_retrieve_secret;
}

static void
secret_dispose (GObject *object)
{
  Secret *secret = (Secret *) object;

  g_clear_object (&secret->impl);

  G_OBJECT_CLASS (secret_parent_class)->dispose (object);
}

static void
secret_init (Secret *secret)
{
}

static void
secret_class_init (SecretClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = secret_dispose;
}

static Secret *
secret_new (XdpContext        *context,
            XdpDbusImplSecret *impl)
{
  Secret *secret;

  secret = g_object_new (secret_get_type (), NULL);
  secret->context = context;
  secret->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (secret->impl), G_MAXINT);

  xdp_dbus_secret_set_version (XDP_DBUS_SECRET (secret), 1);

  return secret;
}

#if HAVE_VARLINK

#define VARLINK_INTERFACE "org.freedesktop.portal.Secret"

static const char varlink_interface_description[] =
  "# The Secret portal allows sandboxed applications to retrieve a\n"
  "# per-application secret. The secret can then be used for encrypting\n"
  "# confidential data inside the sandbox.\n"
  "#\n"
  "# There is no session and no request object: a call needing the user to act\n"
  "# does not reply until it resolves, and closing the connection cancels it\n"
  "interface " VARLINK_INTERFACE "\n"
  "\n"
  "# Retrieves a master secret for a sandboxed application, writing it to\n"
  "# fd_idx. The creation of the file descriptor is the responsibility of the\n"
  "# caller, which passes the write end of a pipe.\n"
  "#\n"
  "# The master secret is unique per application and does not change as long as\n"
  "# the application is installed (once it has been created). In a typical\n"
  "# backend implementation, it is stored in the user's keyring, under the\n"
  "# application ID as a key.\n"
  "#\n"
  "# While the master secret can be used for encrypting any confidential data\n"
  "# in the sandbox, the format is opaque to the application. In particular,\n"
  "# the length of the secret might not be sufficient for the use with certain\n"
  "# encryption algorithm. In that case, the application is supposed to expand\n"
  "# it using a KDF algorithm.\n"
  "#\n"
  "# The portal may return an additional identifier associated with the secret\n"
  "# in token. In the next call of this method, the application shall indicate\n"
  "# it through the token argument\n"
  "method RetrieveSecret(fd_idx: int, token: ?string) -> (token: ?string)\n"
  "\n"
  "# A secret is not available\n"
  "error NoSecret()\n"
  "\n"
  "# The user dismissed the request, as opposed to\n"
  "# org.varlink.service.PermissionDenied for one refused without asking\n"
  "error Cancelled()\n"
  "\n"
  "# The descriptor could not be passed on to the backend\n"
  "error TransferFailed()\n"
  "\n"
  "error " XDP_VARLINK_ERROR_NOT_REGISTERED "()\n";

static long
handle_varlink_retrieve_secret (XdpVarlinkService    *service,
                                XdpVarlinkConnection *connection,
                                VarlinkCall          *call,
                                VarlinkObject        *parameters,
                                uint64_t              flags,
                                gpointer              user_data)
{
  Secret *secret = user_data;
  XdpAppInfo *app_info = xdp_varlink_connection_get_app_info (connection);
  g_autoptr(XdpDbusImplSecretRetrieveSecretResult) result = NULL;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *token = NULL;
  g_autofd int fd = -1;
  int64_t fd_idx;

  if (varlink_object_get_int (parameters, "fd_idx", &fd_idx) < 0)
    return varlink_call_reply_invalid_parameter (call, "fd_idx");

  fd = varlink_call_take_fd (call, fd_idx);
  if (fd < 0)
    return varlink_call_reply_invalid_parameter (call, "fd_idx");

  if (varlink_object_get_string (parameters, "token", &token) >= 0 && token != NULL)
    {
      g_variant_builder_add (&options_builder, "{sv}",
                             "token", g_variant_new_string (token));
    }

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  fd_list = g_unix_fd_list_new ();
  if (g_unix_fd_list_append (fd_list, fd, &error) < 0)
    {
      g_warning ("Failed to pass on the secret fd: %s", error->message);
      return varlink_call_reply_error (call, VARLINK_INTERFACE ".TransferFailed", NULL);
    }

  /* The backend answers on a request object no varlink client sees */
  request = dex_await_object (
    xdp_request_dex_new (secret->context,
                         app_info,
                         G_DBUS_INTERFACE_SKELETON (secret),
                         G_DBUS_PROXY (secret->impl),
                         options),
    &error);
  if (!request)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSecret", NULL);

  result = dex_await_boxed (
    xdp_dbus_impl_secret_call_retrieve_secret_future (
      secret->impl,
      xdp_request_dex_get_object_path (request),
      xdp_app_info_get_id (app_info),
      g_variant_new_handle (0),
      options,
      fd_list),
    &error);

  if (!result)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSecret", NULL);
    }

  if (result->response == XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".Cancelled", NULL);

  if (result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".NoSecret", NULL);

  varlink_object_new (&reply);

  if (result->results &&
      g_variant_lookup (result->results, "token", "&s", &token))
    varlink_object_set_string (reply, "token", token);

  return varlink_call_reply (call, reply, 0);
}

static const XdpVarlinkMethod varlink_methods[] = {
  { "RetrieveSecret", handle_varlink_retrieve_secret, XDP_VARLINK_METHOD_FLAGS_NONE },
};

gboolean
init_secret_varlink (XdpVarlinkService  *service,
                     XdpContext         *context,
                     GError            **error)
{
  GDBusInterfaceSkeleton *skeleton;

  skeleton = xdp_context_get_portal (context, SECRET_DBUS_IFACE);
  if (skeleton == NULL)
    return TRUE;

  return xdp_varlink_service_add_interface (service, varlink_interface_description,
                                            varlink_methods,
                                            G_N_ELEMENTS (varlink_methods),
                                            g_object_ref (skeleton),
                                            g_object_unref, error);
}

#endif /* HAVE_VARLINK */

DexFuture *
init_secret (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(Secret) secret = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplSecret) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SECRET_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_secret_proxy_new_future (
      connection,
      G_DBUS_PROXY_FLAGS_NONE,
      impl_config->dbus_name,
      DESKTOP_DBUS_PATH),
    &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create secret proxy: %s", error->message);
      return dex_future_new_false ();
    }

  secret = secret_new (context, impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&secret)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);
  return dex_future_new_true ();
}
