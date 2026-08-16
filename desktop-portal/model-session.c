/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "model-session.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-utils.h"

#define MODEL_POINT_PROMPTS_TYPE ((const GVariantType *) "a(ddb)")
#define MODEL_BOX_PROMPTS_TYPE ((const GVariantType *) "a(dddd)")

struct _ModelSession
{
  GObject parent_instance;

  XdpSessionDex *session;
  char *use_case;
};

G_DEFINE_FINAL_TYPE (ModelSession, model_session, G_TYPE_OBJECT)

typedef struct
{
  gatomicrefcount ref_count;
  GPtrArray *sealed_fds;
  GUnixFDList *sealed_fd_list;
} ModelRequestResources;

struct _ModelRequest
{
  XdpContext *context;
  XdpRequestDex *request;
  XdpAppInfo *app_info;
  GDBusInterfaceSkeleton *portal;
  GDBusProxy *impl;
  XdpSessionDex *session;
  char *session_handle;
  GArray *signal_handlers;
  DexPromise *terminal;
  DexPromise *closed;
  ModelRequestResources *resources;
  gulong request_closed_handler;
  gulong session_closed_handler;
  gboolean terminal_seen;
  gboolean closed_seen;
  gboolean request_closed_seen;
  gboolean session_closed_seen;
};

static gboolean model_request_is_exported (ModelRequest *request);

static gboolean
validate_execution_mode (const char  *key,
                         GVariant    *value,
                         GVariant    *options,
                         gpointer     user_data,
                         GError     **error)
{
  const char *execution_mode = g_variant_get_string (value, NULL);

  if (g_str_equal (execution_mode, "interactive") ||
      g_str_equal (execution_mode, "background"))
    return TRUE;

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "Unsupported execution mode '%s'",
               execution_mode);
  return FALSE;
}

static const XdpOptionKey response_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "maximum_response_tokens", G_VARIANT_TYPE_INT64, NULL },
  { "temperature", G_VARIANT_TYPE_DOUBLE, NULL },
  { "source_language_hint", G_VARIANT_TYPE_STRING, NULL },
  { "target_language_hint", G_VARIANT_TYPE_STRING, NULL },
  { "execution_mode", G_VARIANT_TYPE_STRING, validate_execution_mode },
};

static const XdpOptionKey token_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "maximum_response_tokens", G_VARIANT_TYPE_INT64, NULL },
  { "temperature", G_VARIANT_TYPE_DOUBLE, NULL },
  { "execution_mode", G_VARIANT_TYPE_STRING, validate_execution_mode },
};

static const XdpOptionKey create_session_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "session_handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static const XdpOptionKey prewarm_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static const XdpOptionKey request_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "execution_mode", G_VARIANT_TYPE_STRING, validate_execution_mode },
};

static const XdpOptionKey speech_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "source_language_hint", G_VARIANT_TYPE_STRING, NULL },
  { "execution_mode", G_VARIANT_TYPE_STRING, validate_execution_mode },
};

static const XdpOptionKey synthesis_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "voice_id", G_VARIANT_TYPE_STRING, NULL },
  { "language_hint", G_VARIANT_TYPE_STRING, NULL },
  { "execution_mode", G_VARIANT_TYPE_STRING, validate_execution_mode },
};

static const XdpOptionKey segment_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "execution_mode", G_VARIANT_TYPE_STRING, validate_execution_mode },
  { "point_prompts", MODEL_POINT_PROMPTS_TYPE, NULL },
  { "box_prompts", MODEL_BOX_PROMPTS_TYPE, NULL },
};

static void
model_session_dispose (GObject *object)
{
  ModelSession *session = MODEL_SESSION (object);

  if (session->session != NULL)
    xdp_session_dex_close (session->session);
  g_clear_object (&session->session);

  G_OBJECT_CLASS (model_session_parent_class)->dispose (object);
}

static void
model_session_finalize (GObject *object)
{
  ModelSession *session = MODEL_SESSION (object);

  g_clear_pointer (&session->use_case, g_free);

  G_OBJECT_CLASS (model_session_parent_class)->finalize (object);
}

static void
model_session_init (ModelSession *session)
{
}

static void
model_session_class_init (ModelSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = model_session_dispose;
  object_class->finalize = model_session_finalize;
}

XdpSessionDexStore *
model_session_store_new (void)
{
  return xdp_session_dex_store_new_wrapped (ModelSession, session);
}

static ModelSession *
model_session_new (XdpSessionDex *session,
                   const char    *use_case)
{
  ModelSession *model_session;

  model_session = g_object_new (MODEL_TYPE_SESSION, NULL);
  model_session->session = g_object_ref (session);
  model_session->use_case = g_strdup (use_case);

  return model_session;
}

ModelSession *
model_session_create (XdpContext             *context,
                      XdpAppInfo             *app_info,
                      GDBusInterfaceSkeleton *portal,
                      GDBusProxy             *impl,
                      const char             *use_case,
                      GVariant               *options,
                      GError                **error)
{
  g_autoptr(XdpSessionDex) session = NULL;

  session = dex_await_object (xdp_session_dex_new (context,
                                                   app_info,
                                                   portal,
                                                   impl,
                                                   options),
                              error);
  if (session == NULL)
    return NULL;

  return model_session_new (session, use_case);
}

ModelSession *
model_session_lookup (XdpSessionDexStore    *store,
                      GDBusMethodInvocation *invocation,
                      const char            *session_handle)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  ModelSession *session;

  session = xdp_session_dex_store_lookup_session (store,
                                                  session_handle,
                                                  app_info);
  if (session == NULL || xdp_session_dex_is_closed (session->session))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return NULL;
    }

  return g_object_ref (session);
}

XdpSessionDex *
model_session_get_session (ModelSession *session)
{
  g_return_val_if_fail (MODEL_IS_SESSION (session), NULL);

  return session->session;
}

gboolean
model_use_case_is_supported (const char         *use_case,
                             const char * const *supported_use_cases)
{
  for (size_t i = 0; supported_use_cases[i] != NULL; i++)
    {
      if (g_str_equal (use_case, supported_use_cases[i]))
        return TRUE;
    }

  return FALSE;
}

GVariant *
model_unsupported_use_case_availability (const char *use_case)
{
  g_autofree char *reason = NULL;

  reason = g_strdup_printf ("Unsupported use-case: %s", use_case);
  return g_variant_ref_sink (g_variant_new ("(bss)",
                                            FALSE,
                                            "unsupported_use_case",
                                            reason));
}

GVariant *
model_get_use_case_availability (GDBusProxy  *impl,
                                 const char  *interface_name,
                                 const char  *app_id,
                                 const char  *use_case,
                                 GError     **error)
{
  g_autoptr(GVariant) reply = NULL;
  GVariant *availability = NULL;

  reply = dex_await_variant (
    dex_dbus_connection_call (g_dbus_proxy_get_connection (impl),
                              g_dbus_proxy_get_name (impl),
                              g_dbus_proxy_get_object_path (impl),
                              interface_name,
                              "GetUseCaseAvailability",
                              g_variant_new ("(ss)", app_id, use_case),
                              G_VARIANT_TYPE ("((bss))"),
                              G_DBUS_CALL_FLAGS_NONE,
                              30000),
    error);
  if (reply == NULL)
    return NULL;

  g_variant_get (reply, "(@(bss))", &availability);
  return availability;
}

gboolean
model_validate_use_case_for_session (GDBusMethodInvocation *invocation,
                                     const char            *use_case,
                                     const char * const    *supported_use_cases)
{
  if (model_use_case_is_supported (use_case, supported_use_cases))
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                         "Unsupported use-case: %s",
                                         use_case);
  return FALSE;
}

gboolean
model_session_ensure_use_case (GDBusMethodInvocation *invocation,
                               ModelSession          *session,
                               const char            *method,
                               const char * const    *allowed_use_cases)
{
  if (model_use_case_is_supported (session->use_case, allowed_use_cases))
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                         "%s is not available for use-case %s",
                                         method,
                                         session->use_case);
  return FALSE;
}

static GVariant *
filter_options (GVariant           *options,
                const XdpOptionKey *allowed_options,
                size_t              n_allowed_options,
                GError            **error)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!xdp_filter_options (options,
                           &builder,
                           allowed_options,
                           n_allowed_options,
                           NULL,
                           error))
    return NULL;

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
validate_token_option (GVariant   *options,
                       const char *key,
                       GError    **error)
{
  const char *token = NULL;

  if (!g_variant_lookup (options, key, "&s", &token))
    return TRUE;

  if (xdp_is_valid_token (token))
    return TRUE;

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "Invalid token '%s'",
               token);
  return FALSE;
}

gboolean
model_availability_options_validate (GVariant  *options,
                                     GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;

  filtered = filter_options (options, NULL, 0, error);
  return filtered != NULL;
}

gboolean
model_session_options_validate (GVariant  *options,
                                 GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;

  filtered = filter_options (options,
                             create_session_options,
                             G_N_ELEMENTS (create_session_options),
                             error);
  if (filtered == NULL)
    return FALSE;

  return validate_token_option (filtered, "handle_token", error) &&
         validate_token_option (filtered, "session_handle_token", error);
}

gboolean
model_prewarm_options_validate (GVariant  *options,
                                GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;

  filtered = filter_options (options,
                             prewarm_options,
                             G_N_ELEMENTS (prewarm_options),
                             error);
  if (filtered == NULL)
    return FALSE;

  return validate_token_option (filtered, "handle_token", error);
}

GVariant *
model_response_options_from_vardict (GVariant  *options,
                                     GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;
  gint64 maximum_response_tokens = 512;
  double temperature = 0.7;
  const char *source_language_hint = "";
  const char *target_language_hint = "";
  const char *execution_mode = "interactive";

  filtered = filter_options (options,
                             response_options,
                             G_N_ELEMENTS (response_options),
                             error);
  if (filtered == NULL)
    return NULL;

  g_variant_lookup (filtered, "maximum_response_tokens", "x", &maximum_response_tokens);
  g_variant_lookup (filtered, "temperature", "d", &temperature);
  g_variant_lookup (filtered, "source_language_hint", "&s", &source_language_hint);
  g_variant_lookup (filtered, "target_language_hint", "&s", &target_language_hint);
  g_variant_lookup (filtered, "execution_mode", "&s", &execution_mode);

  return g_variant_ref_sink (g_variant_new ("(xdsss)",
                                            maximum_response_tokens,
                                            temperature,
                                            source_language_hint,
                                            target_language_hint,
                                            execution_mode));
}

GVariant *
model_token_options_from_vardict (GVariant  *options,
                                  GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;
  gint64 maximum_response_tokens = 512;
  double temperature = 0.7;
  const char *execution_mode = "interactive";

  filtered = filter_options (options,
                             token_options,
                             G_N_ELEMENTS (token_options),
                             error);
  if (filtered == NULL)
    return NULL;

  g_variant_lookup (filtered, "maximum_response_tokens", "x", &maximum_response_tokens);
  g_variant_lookup (filtered, "temperature", "d", &temperature);
  g_variant_lookup (filtered, "execution_mode", "&s", &execution_mode);

  return g_variant_ref_sink (g_variant_new ("(xds)",
                                            maximum_response_tokens,
                                            temperature,
                                            execution_mode));
}

GVariant *
model_request_options_from_vardict (GVariant  *options,
                                    GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;
  const char *execution_mode = "interactive";

  filtered = filter_options (options,
                             request_options,
                             G_N_ELEMENTS (request_options),
                             error);
  if (filtered == NULL)
    return NULL;

  g_variant_lookup (filtered, "execution_mode", "&s", &execution_mode);
  return g_variant_ref_sink (g_variant_new ("(s)", execution_mode));
}

GVariant *
model_speech_options_from_vardict (GVariant  *options,
                                   GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;
  const char *source_language_hint = "";
  const char *execution_mode = "interactive";

  filtered = filter_options (options,
                             speech_options,
                             G_N_ELEMENTS (speech_options),
                             error);
  if (filtered == NULL)
    return NULL;

  g_variant_lookup (filtered, "source_language_hint", "&s", &source_language_hint);
  g_variant_lookup (filtered, "execution_mode", "&s", &execution_mode);

  return g_variant_ref_sink (g_variant_new ("(ss)",
                                            source_language_hint,
                                            execution_mode));
}

GVariant *
model_synthesis_options_from_vardict (GVariant  *options,
                                      GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;
  const char *voice_id = "";
  const char *language_hint = "";
  const char *execution_mode = "interactive";

  filtered = filter_options (options,
                             synthesis_options,
                             G_N_ELEMENTS (synthesis_options),
                             error);
  if (filtered == NULL)
    return NULL;

  g_variant_lookup (filtered, "voice_id", "&s", &voice_id);
  g_variant_lookup (filtered, "language_hint", "&s", &language_hint);
  g_variant_lookup (filtered, "execution_mode", "&s", &execution_mode);

  return g_variant_ref_sink (g_variant_new ("(sss)",
                                            voice_id,
                                            language_hint,
                                            execution_mode));
}

GVariant *
model_segment_options_from_vardict (GVariant  *options,
                                    GError   **error)
{
  g_autoptr(GVariant) filtered = NULL;
  g_autoptr(GVariant) point_prompts = NULL;
  g_autoptr(GVariant) box_prompts = NULL;
  const char *execution_mode = "interactive";

  filtered = filter_options (options,
                             segment_options,
                             G_N_ELEMENTS (segment_options),
                             error);
  if (filtered == NULL)
    return NULL;

  g_variant_lookup (filtered, "execution_mode", "&s", &execution_mode);
  point_prompts = g_variant_lookup_value (filtered,
                                          "point_prompts",
                                          G_VARIANT_TYPE ("a(ddb)"));
  box_prompts = g_variant_lookup_value (filtered,
                                        "box_prompts",
                                        G_VARIANT_TYPE ("a(dddd)"));

  if (point_prompts == NULL)
    point_prompts = g_variant_ref_sink (
      g_variant_new_array (G_VARIANT_TYPE ("(ddb)"), NULL, 0));
  if (box_prompts == NULL)
    box_prompts = g_variant_ref_sink (
      g_variant_new_array (G_VARIANT_TYPE ("(dddd)"), NULL, 0));

  return g_variant_ref_sink (g_variant_new ("(s@a(ddb)@a(dddd))",
                                            execution_mode,
                                            point_prompts,
                                            box_prompts));
}

static void
set_invalid_fd_error (GError  **error,
                      GError   *cause)
{
  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "Invalid file descriptor: %s",
               cause->message);
}

gboolean
model_seal_fd (GVariant      *handle,
               GUnixFDList   *fd_list,
               GVariant     **sealed_handle_out,
               GUnixFDList  **sealed_fd_list_out,
               XdpSealedFd  **sealed_fd_out,
               GError       **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = g_unix_fd_list_new ();
  g_autoptr(XdpSealedFd) sealed_fd = NULL;
  g_autoptr(GVariant) sealed_handle = NULL;
  int fd_index;

  sealed_fd = xdp_sealed_fd_new_from_handle (handle, fd_list, &local_error);
  if (sealed_fd == NULL)
    {
      set_invalid_fd_error (error, local_error);
      return FALSE;
    }

  fd_index = g_unix_fd_list_append (sealed_fd_list,
                                    xdp_sealed_fd_get_fd (sealed_fd),
                                    &local_error);
  if (fd_index == -1)
    {
      set_invalid_fd_error (error, local_error);
      return FALSE;
    }

  sealed_handle = g_variant_ref_sink (g_variant_new_handle (fd_index));
  *sealed_handle_out = g_steal_pointer (&sealed_handle);
  *sealed_fd_list_out = g_steal_pointer (&sealed_fd_list);
  *sealed_fd_out = g_steal_pointer (&sealed_fd);
  return TRUE;
}

gboolean
model_seal_fds (GVariant      *handles,
                GUnixFDList   *fd_list,
                GVariant     **sealed_handles_out,
                GUnixFDList  **sealed_fd_list_out,
                GPtrArray    **sealed_fds_out,
                GError       **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = g_unix_fd_list_new ();
  g_autoptr(GPtrArray) sealed_fds =
    g_ptr_array_new_with_free_func (g_object_unref);
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("ah"));
  GVariantIter iter;
  gint32 handle;

  g_variant_iter_init (&iter, handles);
  while (g_variant_iter_next (&iter, "h", &handle))
    {
      g_autoptr(GVariant) handle_variant = NULL;
      g_autoptr(XdpSealedFd) sealed_fd = NULL;
      int fd_index;

      handle_variant = g_variant_ref_sink (g_variant_new_handle (handle));
      sealed_fd = xdp_sealed_fd_new_from_handle (handle_variant,
                                                 fd_list,
                                                 &local_error);
      if (sealed_fd == NULL)
        {
          set_invalid_fd_error (error, local_error);
          return FALSE;
        }

      fd_index = g_unix_fd_list_append (sealed_fd_list,
                                        xdp_sealed_fd_get_fd (sealed_fd),
                                        &local_error);
      if (fd_index == -1)
        {
          set_invalid_fd_error (error, local_error);
          return FALSE;
        }

      g_variant_builder_add (&builder, "h", fd_index);
      g_ptr_array_add (sealed_fds, g_steal_pointer (&sealed_fd));
    }

  *sealed_handles_out = g_variant_ref_sink (g_variant_builder_end (&builder));
  *sealed_fd_list_out = g_steal_pointer (&sealed_fd_list);
  *sealed_fds_out = g_steal_pointer (&sealed_fds);
  return TRUE;
}

static void
model_request_on_closed (GObject      *object,
                         ModelRequest *request)
{
  if (object == G_OBJECT (request->request))
    request->request_closed_seen = TRUE;
  else if (object == G_OBJECT (request->session))
    request->session_closed_seen = TRUE;

  if (request->closed_seen)
    return;

  request->closed_seen = TRUE;
  dex_promise_reject (request->closed,
                      g_error_new_literal (XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_CANCELLED,
                                           "Request closed"));
}

ModelRequest *
model_request_new (XdpContext             *context,
                   XdpAppInfo             *app_info,
                   GDBusInterfaceSkeleton *portal,
                   GDBusProxy             *impl,
                   XdpSessionDex          *session,
                   GVariant               *options,
                   GError                **error)
{
  g_autoptr(XdpRequestDex) request = NULL;
  ModelRequest *model_request;

  if (xdp_session_dex_is_closed (session))
    {
      g_set_error_literal (error,
                           G_DBUS_ERROR,
                           G_DBUS_ERROR_ACCESS_DENIED,
                           "Invalid session");
      return NULL;
    }

  request = dex_await_object (xdp_request_dex_new (context,
                                                   app_info,
                                                   portal,
                                                   impl,
                                                   options),
                              error);
  if (request == NULL)
    return NULL;

  model_request = g_new0 (ModelRequest, 1);
  model_request->context = context;
  model_request->request = g_steal_pointer (&request);
  model_request->app_info = g_object_ref (app_info);
  model_request->portal = g_object_ref (portal);
  model_request->impl = g_object_ref (impl);
  model_request->session = g_object_ref (session);
  model_request->session_handle =
    g_strdup (xdp_session_dex_get_object_path (session));
  model_request->signal_handlers = g_array_new (FALSE, FALSE, sizeof (gulong));
  model_request->terminal = dex_promise_new ();
  model_request->closed = dex_promise_new ();
  model_request->request_closed_handler =
    g_signal_connect (model_request->request,
                      "request-closed",
                      G_CALLBACK (model_request_on_closed),
                      model_request);
  model_request->session_closed_handler =
    g_signal_connect (model_request->session,
                      "session-closed",
                      G_CALLBACK (model_request_on_closed),
                      model_request);

  if (!model_request_is_exported (model_request) ||
      xdp_session_dex_is_closed (model_request->session))
    {
      g_set_error_literal (error,
                           G_DBUS_ERROR,
                           G_DBUS_ERROR_ACCESS_DENIED,
                           "Invalid session");
      model_request_free (model_request);
      return NULL;
    }

  return model_request;
}

static ModelRequestResources *
model_request_resources_new (void)
{
  ModelRequestResources *resources;

  resources = g_new0 (ModelRequestResources, 1);
  g_atomic_ref_count_init (&resources->ref_count);
  return resources;
}

static ModelRequestResources *
model_request_resources_ref (ModelRequestResources *resources)
{
  g_atomic_ref_count_inc (&resources->ref_count);
  return resources;
}

static void
model_request_resources_unref (ModelRequestResources *resources)
{
  if (!g_atomic_ref_count_dec (&resources->ref_count))
    return;

  g_clear_pointer (&resources->sealed_fds, g_ptr_array_unref);
  g_clear_object (&resources->sealed_fd_list);
  g_free (resources);
}

void
model_request_free (ModelRequest *request)
{
  if (request == NULL)
    return;

  for (size_t i = 0; i < request->signal_handlers->len; i++)
    {
      gulong handler_id = g_array_index (request->signal_handlers, gulong, i);

      if (g_signal_handler_is_connected (request->impl, handler_id))
        g_signal_handler_disconnect (request->impl, handler_id);
    }

  g_clear_signal_handler (&request->request_closed_handler, request->request);
  g_clear_signal_handler (&request->session_closed_handler, request->session);
  xdp_request_dex_close (request->request);

  g_clear_pointer (&request->signal_handlers, g_array_unref);
  g_clear_pointer (&request->terminal, dex_unref);
  g_clear_pointer (&request->closed, dex_unref);
  g_clear_pointer (&request->resources, model_request_resources_unref);
  g_clear_object (&request->request);
  g_clear_object (&request->session);
  g_clear_object (&request->app_info);
  g_clear_object (&request->portal);
  g_clear_object (&request->impl);
  g_clear_pointer (&request->session_handle, g_free);
  g_free (request);
}

const char *
model_request_get_handle (ModelRequest *request)
{
  return xdp_request_dex_get_object_path (request->request);
}

const char *
model_request_get_session_handle (ModelRequest *request)
{
  return request->session_handle;
}

static gboolean
model_request_is_exported (ModelRequest *request)
{
  return g_dbus_interface_skeleton_get_connection (
           G_DBUS_INTERFACE_SKELETON (request->request)) != NULL;
}

gboolean
model_request_matches (ModelRequest *request,
                       const char   *request_handle,
                       const char   *session_handle)
{
  return !request->terminal_seen &&
         !request->closed_seen &&
         model_request_is_exported (request) &&
         !xdp_session_dex_is_closed (request->session) &&
         g_str_equal (model_request_get_handle (request), request_handle) &&
         g_str_equal (request->session_handle, session_handle);
}

void
model_request_connect_signal (ModelRequest *request,
                              const char   *detailed_signal,
                              GCallback     callback)
{
  gulong handler_id;

  handler_id = g_signal_connect (request->impl,
                                 detailed_signal,
                                 callback,
                                 request);
  g_array_append_val (request->signal_handlers, handler_id);
}

void
model_request_emit_signal (ModelRequest *request,
                           const char   *signal_name,
                           GVariant     *parameters)
{
  g_autoptr(GVariant) owned_parameters = g_variant_ref_sink (parameters);
  GDBusConnection *connection;
  const char *interface_name;

  if (!model_request_is_exported (request))
    return;

  connection = g_dbus_interface_skeleton_get_connection (request->portal);
  if (connection == NULL)
    return;

  interface_name = g_dbus_interface_skeleton_get_info (request->portal)->name;
  g_dbus_connection_emit_signal (connection,
                                 xdp_app_info_get_sender (request->app_info),
                                 DESKTOP_DBUS_PATH,
                                 interface_name,
                                 signal_name,
                                 owned_parameters,
                                 NULL);
}

static void
forward_model_loading (GObject      *impl,
                       const char   *request_handle,
                       const char   *session_handle,
                       const char   *message,
                       ModelRequest *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "ModelLoading",
                             g_variant_new ("(oos)",
                                            request_handle,
                                            session_handle,
                                            message));
}

void
model_request_connect_loading (ModelRequest *request)
{
  model_request_connect_signal (request,
                                "model-loading",
                                G_CALLBACK (forward_model_loading));
}

void
model_request_mark_terminal (ModelRequest *request)
{
  if (request->terminal_seen)
    return;

  request->terminal_seen = TRUE;
  dex_promise_resolve_boolean (request->terminal, TRUE);
}

void
model_request_take_sealed_fd (ModelRequest *request,
                              XdpSealedFd  *sealed_fd,
                              GUnixFDList  *sealed_fd_list)
{
  g_return_if_fail (request->resources == NULL);

  request->resources = model_request_resources_new ();
  request->resources->sealed_fds =
    g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (request->resources->sealed_fds, sealed_fd);
  request->resources->sealed_fd_list = sealed_fd_list;
}

void
model_request_take_sealed_fds (ModelRequest *request,
                               GPtrArray    *sealed_fds,
                               GUnixFDList  *sealed_fd_list)
{
  g_return_if_fail (request->resources == NULL);

  request->resources = model_request_resources_new ();
  request->resources->sealed_fds = sealed_fds;
  request->resources->sealed_fd_list = sealed_fd_list;
}

static gboolean
error_is_cancelled (GError *error)
{
  g_autofree char *remote_error = NULL;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_CANCELLED))
    return TRUE;

  remote_error = g_dbus_error_get_remote_error (error);
  return g_strcmp0 (remote_error, "org.freedesktop.portal.Error.Cancelled") == 0;
}

static DexFuture *
model_request_call_completed (DexFuture *future G_GNUC_UNUSED,
                              gpointer   user_data G_GNUC_UNUSED)
{
  return dex_future_new_true ();
}

void
model_request_emit_error (ModelRequest *request,
                          GError       *error)
{
  g_auto(GVariantBuilder) results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GError) public_error = g_error_copy (error);
  g_autofree char *remote_error = NULL;
  XdgDesktopPortalResponseEnum response;

  if (!model_request_is_exported (request))
    return;

  response = error_is_cancelled (error)
               ? XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED
               : XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  if (g_dbus_error_is_remote_error (error))
    {
      remote_error = g_dbus_error_get_remote_error (error);
      g_dbus_error_strip_remote_error (public_error);
    }

  g_variant_builder_add (&results,
                         "{sv}",
                         "error",
                         g_variant_new_string (public_error->message));
  if (remote_error != NULL)
    g_variant_builder_add (&results,
                           "{sv}",
                           "error_name",
                           g_variant_new_string (remote_error));

  xdp_request_dex_emit_response (request->request,
                                 response,
                                  g_variant_builder_end (&results));
}

static gboolean
model_request_cancel_if_session_closed (ModelRequest *request)
{
  g_autoptr(GError) error = NULL;

  if (!request->session_closed_seen &&
      !xdp_session_dex_is_closed (request->session))
    return FALSE;

  error = g_error_new_literal (XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_CANCELLED,
                               "Session closed");
  model_request_emit_error (request, error);
  return TRUE;
}

gboolean
model_request_await_call (ModelRequest *request,
                          DexFuture    *call_future)
{
  g_autoptr(GError) error = NULL;
  DexFuture *future;

  if (request->resources != NULL)
    dex_future_disown (
      dex_future_finally (dex_ref (call_future),
                          model_request_call_completed,
                          model_request_resources_ref (request->resources),
                          (GDestroyNotify) model_request_resources_unref));

  future = dex_future_first (call_future,
                             dex_ref (DEX_FUTURE (request->closed)),
                             NULL);
  if (!dex_await (future, &error))
    {
      if (!request->closed_seen &&
          xdp_context_is_cancelled (request->context))
        xdp_request_dex_close (request->request);
      else
        model_request_emit_error (request, error);
      return FALSE;
    }

  if (request->request_closed_seen || !model_request_is_exported (request))
    return FALSE;

  return !model_request_cancel_if_session_closed (request);
}

void
model_request_finish (ModelRequest *request,
                      DexFuture    *call_future,
                      gboolean      wait_for_terminal)
{
  g_autoptr(GError) error = NULL;
  DexFuture *future;

  if (!model_request_await_call (request, call_future))
    return;

  if (wait_for_terminal && !request->terminal_seen)
    {
      future = dex_future_first (dex_ref (DEX_FUTURE (request->terminal)),
                                 dex_ref (DEX_FUTURE (request->closed)),
                                 NULL);
      if (!dex_await (future, &error))
        {
          if (!request->closed_seen &&
              xdp_context_is_cancelled (request->context))
            xdp_request_dex_close (request->request);
          else
            model_request_emit_error (request, error);
          return;
        }
    }

  if (request->request_closed_seen || !model_request_is_exported (request))
    return;

  if (model_request_cancel_if_session_closed (request))
    return;

  xdp_request_dex_emit_response (request->request,
                                 XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                 NULL);
}

gboolean
model_request_emit_session_response (ModelRequest  *request,
                                     XdpSessionDex *session)
{
  g_auto(GVariantBuilder) results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!model_request_is_exported (request))
    return FALSE;

  if (model_request_cancel_if_session_closed (request))
    return FALSE;

  g_variant_builder_add (&results,
                         "{sv}",
                         "session_handle",
                         g_variant_new_object_path (
                           xdp_session_dex_get_object_path (session)));
  xdp_request_dex_emit_response (request->request,
                                 XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                 g_variant_builder_end (&results));
  return TRUE;
}
