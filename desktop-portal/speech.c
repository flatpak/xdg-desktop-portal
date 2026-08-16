/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "speech.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

typedef struct _Speech Speech;
typedef struct _SpeechClass SpeechClass;

struct _Speech
{
  XdpDbusSpeechSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplSpeech *impl;
  XdpSessionDexStore *sessions;
};

struct _SpeechClass
{
  XdpDbusSpeechSkeletonClass parent_class;
};

GType speech_get_type (void);

static void speech_iface_init (XdpDbusSpeechIface *iface);

G_DEFINE_TYPE_WITH_CODE (Speech, speech, XDP_DBUS_TYPE_SPEECH_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH,
                                                speech_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Speech, g_object_unref)

static const char * const speech_use_cases[] = {
  "speech.transcribe",
  "speech.translate",
  "speech.synthesize",
  NULL,
};

static const char * const speech_transcribe_use_cases[] = {
  "speech.transcribe",
  "speech.translate",
  NULL,
};

static const char * const speech_synthesize_use_cases[] = {
  "speech.synthesize",
  NULL,
};

static void
forward_transcription_received (XdpDbusImplSpeech *impl G_GNUC_UNUSED,
                                const char        *request_handle,
                                const char        *session_handle,
                                const char        *text,
                                gboolean           done,
                                ModelRequest      *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "TranscriptionReceived",
                             g_variant_new ("(oosb)",
                                            request_handle,
                                            session_handle,
                                            text,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_audio_received (XdpDbusImplSpeech *impl G_GNUC_UNUSED,
                        const char        *request_handle,
                        const char        *session_handle,
                        GVariant          *audio,
                        guint              sample_rate,
                        guint              channels,
                        const char        *sample_format,
                        gboolean           done,
                        ModelRequest      *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "AudioReceived",
                             g_variant_new ("(oo@ayuusb)",
                                            request_handle,
                                            session_handle,
                                            audio,
                                            sample_rate,
                                            channels,
                                            sample_format,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static gboolean
handle_speech_get_use_case_availability (XdpDbusSpeech       *object,
                                         GDBusMethodInvocation *invocation,
                                         const char            *arg_use_case,
                                         GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!model_availability_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_use_case_is_supported (arg_use_case, speech_use_cases))
    {
      availability = model_unsupported_use_case_availability (arg_use_case);

      xdp_dbus_speech_complete_get_use_case_availability (
        object,
        invocation,
        availability);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  availability = model_get_use_case_availability (
    G_DBUS_PROXY (speech->impl),
    SPEECH_DBUS_IMPL_IFACE,
    xdp_app_info_get_id (app_info),
    arg_use_case,
    &error);
  if (availability == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_get_use_case_availability (
    object,
    invocation,
    availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_create_session (XdpDbusSpeech       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_parent_window,
                              const char            *arg_use_case,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GError) error = NULL;
  XdpSessionDex *session_dex;
  DexFuture *call_future;

  if (!model_validate_use_case_for_session (invocation,
                                            arg_use_case,
                                            speech_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = model_session_create (speech->context,
                                  app_info,
                                  G_DBUS_INTERFACE_SKELETON (speech),
                                  G_DBUS_PROXY (speech->impl),
                                  arg_use_case,
                                  arg_options,
                                  &error);
  if (session == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session_dex = model_session_get_session (session);
  request = model_request_new (speech->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (speech),
                               G_DBUS_PROXY (speech->impl),
                               session_dex,
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  call_future = xdp_dbus_impl_speech_call_create_session_future (
    speech->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    xdp_app_info_get_id (app_info),
    arg_parent_window,
    arg_use_case,
    arg_instructions);
  xdp_dbus_speech_complete_create_session (object,
                                           invocation,
                                           model_request_get_handle (request));

  if (!model_request_await_call (request, call_future))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (model_request_emit_session_response (request, session_dex))
    xdp_session_dex_store_take_session (speech->sessions,
                                        g_steal_pointer (&session));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_prewarm (XdpDbusSpeech       *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_session_handle,
                       GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (speech->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_prewarm_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (speech->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (speech),
                               G_DBUS_PROXY (speech->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  call_future = xdp_dbus_impl_speech_call_prewarm_future (
    speech->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request));
  xdp_dbus_speech_complete_prewarm (object,
                                    invocation,
                                    model_request_get_handle (request));
  model_request_finish (request, call_future, FALSE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_stream_transcribe (XdpDbusSpeech       *object,
                                 GDBusMethodInvocation *invocation,
                                 GUnixFDList           *fd_list,
                                 const char            *arg_session_handle,
                                 GVariant              *arg_audio_fd,
                                 GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_audio_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_audio = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (speech->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamTranscribe",
                                      speech_transcribe_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_speech_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_audio_fd,
                      fd_list,
                      &sealed_audio_fd,
                      &sealed_fd_list,
                      &sealed_audio,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (speech->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (speech),
                               G_DBUS_PROXY (speech->impl),
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
                                "transcription-received",
                                G_CALLBACK (forward_transcription_received));
  call_future = xdp_dbus_impl_speech_call_stream_transcribe_future (
    speech->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_audio_fd,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_audio),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_speech_complete_stream_transcribe (
    object,
    invocation,
    NULL,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_stream_synthesize (XdpDbusSpeech       *object,
                                 GDBusMethodInvocation *invocation,
                                 const char            *arg_session_handle,
                                 const char            *arg_text,
                                 GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (speech->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamSynthesize",
                                      speech_synthesize_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (arg_text[0] == '\0')
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Synthesis text must not be empty");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = model_synthesis_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (speech->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (speech),
                               G_DBUS_PROXY (speech->impl),
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
                                "audio-received",
                                G_CALLBACK (forward_audio_received));
  call_future = xdp_dbus_impl_speech_call_stream_synthesize_future (
    speech->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    arg_text,
    options);
  xdp_dbus_speech_complete_stream_synthesize (
    object,
    invocation,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
speech_iface_init (XdpDbusSpeechIface *iface)
{
  iface->handle_get_use_case_availability = handle_speech_get_use_case_availability;
  iface->handle_create_session = handle_speech_create_session;
  iface->handle_prewarm = handle_speech_prewarm;
  iface->handle_stream_transcribe = handle_speech_stream_transcribe;
  iface->handle_stream_synthesize = handle_speech_stream_synthesize;
}

static void
speech_dispose (GObject *object)
{
  Speech *speech = (Speech *) object;

  g_clear_object (&speech->sessions);
  g_clear_object (&speech->impl);

  G_OBJECT_CLASS (speech_parent_class)->dispose (object);
}

static void
speech_init (Speech *speech)
{
}

static void
speech_class_init (SpeechClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = speech_dispose;
}

static Speech *
speech_new (XdpContext        *context,
            XdpDbusImplSpeech *impl)
{
  Speech *speech;

  speech = g_object_new (speech_get_type (), NULL);
  speech->context = context;
  speech->impl = g_object_ref (impl);
  speech->sessions = model_session_store_new ();

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (speech->impl), G_MAXINT);
  xdp_dbus_speech_set_version (XDP_DBUS_SPEECH (speech), 1);

  return speech;
}

DexFuture *
init_speech (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(Speech) speech = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplSpeech) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SPEECH_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_speech_proxy_new_future (
      connection,
      G_DBUS_PROXY_FLAGS_NONE,
      impl_config->dbus_name,
      DESKTOP_DBUS_PATH),
    &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create speech proxy: %s", error->message);
      return dex_future_new_false ();
    }

  speech = speech_new (context, impl);
  xdp_context_take_and_export_portal (
    context,
    G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&speech)),
    XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
