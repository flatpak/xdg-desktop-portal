/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "language.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

typedef struct _Language Language;
typedef struct _LanguageClass LanguageClass;

struct _Language
{
  XdpDbusLanguageSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplLanguage *impl;
  XdpSessionDexStore *sessions;
};

struct _LanguageClass
{
  XdpDbusLanguageSkeletonClass parent_class;
};

GType language_get_type (void);

static void language_iface_init (XdpDbusLanguageIface *iface);

G_DEFINE_TYPE_WITH_CODE (Language, language, XDP_DBUS_TYPE_LANGUAGE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_LANGUAGE,
                                                language_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Language, g_object_unref)

static const char * const language_use_cases[] = {
  "language.summarize",
  "language.translate",
  "language.rephrase",
  "language.classify",
  "language.extract",
  "language.analyze",
  "language.embed",
  NULL,
};

static const char * const language_generation_use_cases[] = {
  "language.summarize",
  "language.translate",
  "language.rephrase",
  "language.classify",
  "language.extract",
  "language.analyze",
  NULL,
};

static const char * const language_embed_use_cases[] = {
  "language.embed",
  NULL,
};

static void
forward_token_received (XdpDbusImplLanguage *impl G_GNUC_UNUSED,
                        const char          *request_handle,
                        const char          *session_handle,
                        const char          *token,
                        gboolean             done,
                        ModelRequest        *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "TokenReceived",
                             g_variant_new ("(oosb)",
                                            request_handle,
                                            session_handle,
                                            token,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_guided_snapshot_received (XdpDbusImplLanguage *impl G_GNUC_UNUSED,
                                  const char          *request_handle,
                                  const char          *session_handle,
                                  const char          *snapshot_json,
                                  gboolean             done,
                                  ModelRequest        *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "GuidedSnapshotReceived",
                             g_variant_new ("(oosb)",
                                            request_handle,
                                            session_handle,
                                            snapshot_json,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_guided_tool_calls_received (XdpDbusImplLanguage *impl G_GNUC_UNUSED,
                                    const char          *request_handle,
                                    const char          *session_handle,
                                    GVariant            *tool_calls,
                                    gboolean             done,
                                    ModelRequest        *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "GuidedToolCallsReceived",
                             g_variant_new ("(oo@a(sss)b)",
                                            request_handle,
                                            session_handle,
                                            tool_calls,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_embedding_received (XdpDbusImplLanguage *impl G_GNUC_UNUSED,
                            const char          *request_handle,
                            const char          *session_handle,
                            GVariant            *embedding,
                            const char          *embedding_pipeline_id,
                            gboolean             done,
                            ModelRequest        *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "EmbeddingReceived",
                             g_variant_new ("(oo@adsb)",
                                            request_handle,
                                            session_handle,
                                            embedding,
                                            embedding_pipeline_id,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static gboolean
handle_language_get_use_case_availability (XdpDbusLanguage       *object,
                                           GDBusMethodInvocation *invocation,
                                           const char            *arg_use_case,
                                           GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!model_availability_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_use_case_is_supported (arg_use_case, language_use_cases))
    {
      availability = model_unsupported_use_case_availability (arg_use_case);

      xdp_dbus_language_complete_get_use_case_availability (
        object,
        invocation,
        availability);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  availability = model_get_use_case_availability (
    G_DBUS_PROXY (language->impl),
    LANGUAGE_DBUS_IMPL_IFACE,
    xdp_app_info_get_id (app_info),
    arg_use_case,
    &error);
  if (availability == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_get_use_case_availability (
    object,
    invocation,
    availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_create_session (XdpDbusLanguage       *object,
                                GDBusMethodInvocation *invocation,
                                const char            *arg_parent_window,
                                const char            *arg_use_case,
                                const char            *arg_instructions,
                                GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GError) error = NULL;
  XdpSessionDex *session_dex;
  DexFuture *call_future;

  if (!model_validate_use_case_for_session (invocation,
                                            arg_use_case,
                                            language_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = model_session_create (language->context,
                                  app_info,
                                  G_DBUS_INTERFACE_SKELETON (language),
                                  G_DBUS_PROXY (language->impl),
                                  arg_use_case,
                                  arg_options,
                                  &error);
  if (session == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session_dex = model_session_get_session (session);
  request = model_request_new (language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (language),
                               G_DBUS_PROXY (language->impl),
                               session_dex,
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  call_future = xdp_dbus_impl_language_call_create_session_future (
    language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    xdp_app_info_get_id (app_info),
    arg_parent_window,
    arg_use_case,
    arg_instructions);
  xdp_dbus_language_complete_create_session (object,
                                             invocation,
                                             model_request_get_handle (request));

  if (!model_request_await_call (request, call_future))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (model_request_emit_session_response (request, session_dex))
    xdp_session_dex_store_take_session (language->sessions,
                                        g_steal_pointer (&session));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_prewarm (XdpDbusLanguage       *object,
                         GDBusMethodInvocation *invocation,
                         const char            *arg_session_handle,
                         GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_prewarm_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (language),
                               G_DBUS_PROXY (language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  call_future = xdp_dbus_impl_language_call_prewarm_future (
    language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request));
  xdp_dbus_language_complete_prewarm (object,
                                      invocation,
                                      model_request_get_handle (request));
  model_request_finish (request, call_future, FALSE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_response (XdpDbusLanguage       *object,
                                 GDBusMethodInvocation *invocation,
                                 GUnixFDList           *fd_list,
                                 const char            *arg_session_handle,
                                 const char            *arg_input_json,
                                 GVariant              *arg_media_fds,
                                 GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_media_fds = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(GPtrArray) sealed_fds = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamResponse",
                                      language_generation_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_response_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fds (arg_media_fds,
                       fd_list,
                       &sealed_media_fds,
                       &sealed_fd_list,
                       &sealed_fds,
                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (language),
                               G_DBUS_PROXY (language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (request,
                                "token-received",
                                G_CALLBACK (forward_token_received));
  call_future = xdp_dbus_impl_language_call_stream_response_future (
    language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    arg_input_json,
    sealed_media_fds,
    options,
    sealed_fd_list);
  model_request_take_sealed_fds (request,
                                  g_steal_pointer (&sealed_fds),
                                  g_steal_pointer (&sealed_fd_list));
  xdp_dbus_language_complete_stream_response (
    object,
    invocation,
    NULL,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_respond_guided (XdpDbusLanguage       *object,
                                       GDBusMethodInvocation *invocation,
                                       GUnixFDList           *fd_list,
                                       const char            *arg_session_handle,
                                       const char            *arg_prompt,
                                       GVariant              *arg_media_fds,
                                       GVariant              *arg_fields,
                                       GVariant              *arg_tools,
                                       GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_media_fds = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(GPtrArray) sealed_fds = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamRespondGuided",
                                      language_generation_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_token_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fds (arg_media_fds,
                       fd_list,
                       &sealed_media_fds,
                       &sealed_fd_list,
                       &sealed_fds,
                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (language),
                               G_DBUS_PROXY (language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (request,
                                "guided-snapshot-received",
                                G_CALLBACK (forward_guided_snapshot_received));
  model_request_connect_signal (request,
                                "guided-tool-calls-received",
                                G_CALLBACK (forward_guided_tool_calls_received));
  call_future = xdp_dbus_impl_language_call_stream_respond_guided_future (
    language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    arg_prompt,
    sealed_media_fds,
    arg_fields,
    arg_tools,
    options,
    sealed_fd_list);
  model_request_take_sealed_fds (request,
                                  g_steal_pointer (&sealed_fds),
                                  g_steal_pointer (&sealed_fd_list));
  xdp_dbus_language_complete_stream_respond_guided (
    object,
    invocation,
    NULL,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_submit_tool_results_guided (
  XdpDbusLanguage       *object,
  GDBusMethodInvocation *invocation,
  GUnixFDList           *fd_list,
  const char            *arg_session_handle,
  const char            *arg_prompt,
  GVariant              *arg_media_fds,
  GVariant              *arg_results,
  GVariant              *arg_fields,
  GVariant              *arg_tools,
  GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_media_fds = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(GPtrArray) sealed_fds = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (
        invocation,
        session,
        "StreamSubmitToolResultsGuided",
        language_generation_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_token_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fds (arg_media_fds,
                       fd_list,
                       &sealed_media_fds,
                       &sealed_fd_list,
                       &sealed_fds,
                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (language),
                               G_DBUS_PROXY (language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (request,
                                "guided-snapshot-received",
                                G_CALLBACK (forward_guided_snapshot_received));
  model_request_connect_signal (request,
                                "guided-tool-calls-received",
                                G_CALLBACK (forward_guided_tool_calls_received));
  call_future =
    xdp_dbus_impl_language_call_stream_submit_tool_results_guided_future (
      language->impl,
      model_request_get_handle (request),
      model_request_get_session_handle (request),
      arg_prompt,
      sealed_media_fds,
      arg_results,
      arg_fields,
      arg_tools,
      options,
      sealed_fd_list);
  model_request_take_sealed_fds (request,
                                  g_steal_pointer (&sealed_fds),
                                  g_steal_pointer (&sealed_fd_list));
  xdp_dbus_language_complete_stream_submit_tool_results_guided (
    object,
    invocation,
    NULL,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_embed (XdpDbusLanguage       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_session_handle,
                              const char            *arg_text,
                              GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamEmbed",
                                      language_embed_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_request_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (language),
                               G_DBUS_PROXY (language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (request,
                                "embedding-received",
                                G_CALLBACK (forward_embedding_received));
  call_future = xdp_dbus_impl_language_call_stream_embed_future (
    language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    arg_text,
    options);
  xdp_dbus_language_complete_stream_embed (object,
                                           invocation,
                                           model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
language_iface_init (XdpDbusLanguageIface *iface)
{
  iface->handle_get_use_case_availability = handle_language_get_use_case_availability;
  iface->handle_create_session = handle_language_create_session;
  iface->handle_prewarm = handle_language_prewarm;
  iface->handle_stream_response = handle_language_stream_response;
  iface->handle_stream_respond_guided = handle_language_stream_respond_guided;
  iface->handle_stream_submit_tool_results_guided = handle_language_stream_submit_tool_results_guided;
  iface->handle_stream_embed = handle_language_stream_embed;
}

static void
language_dispose (GObject *object)
{
  Language *language = (Language *) object;

  g_clear_object (&language->sessions);
  g_clear_object (&language->impl);

  G_OBJECT_CLASS (language_parent_class)->dispose (object);
}

static void
language_init (Language *language)
{
}

static void
language_class_init (LanguageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = language_dispose;
}

static Language *
language_new (XdpContext          *context,
              XdpDbusImplLanguage *impl)
{
  Language *language;

  language = g_object_new (language_get_type (), NULL);
  language->context = context;
  language->impl = g_object_ref (impl);
  language->sessions = model_session_store_new ();

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (language->impl), G_MAXINT);
  xdp_dbus_language_set_version (XDP_DBUS_LANGUAGE (language), 1);

  return language;
}

DexFuture *
init_language (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(Language) language = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplLanguage) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, LANGUAGE_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_language_proxy_new_future (
      connection,
      G_DBUS_PROXY_FLAGS_NONE,
      impl_config->dbus_name,
      DESKTOP_DBUS_PATH),
    &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create language proxy: %s", error->message);
      return dex_future_new_false ();
    }

  language = language_new (context, impl);
  xdp_context_take_and_export_portal (
    context,
    G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&language)),
    XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
