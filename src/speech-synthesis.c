/*
 * Copyright © 2025 GNOME Foundation Inc.
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
 * Authors:
 *       Eitan Isaacson <eitan@monotonous.org>
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib-unix.h>

#include <spiel.h>

#include "speech-synthesis.h"
#include "xdp-dbus.h"
#include "xdp-permissions.h"
#include "xdp-request.h"
#include "xdp-session.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "speech-synthesis"
#define PERMISSION_ID "speech-synthesis"

static GQuark quark_request_session;

typedef struct
{
  XdpSession parent;

  SpielRegistry *registry;
  GPtrArray *providers;
} SpeechSynthesisSession;

typedef struct
{
  XdpSessionClass parent_class;
} SpeechSynthesisSessionClass;

GType speech_synthesis_session_get_type (void);

static void speech_synthesis_session_async_initable_iface_init (
    GAsyncInitableIface *async_initable_iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
    SpeechSynthesisSession,
    speech_synthesis_session,
    xdp_session_get_type (),
    G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                           speech_synthesis_session_async_initable_iface_init))

G_GNUC_UNUSED static inline SpeechSynthesisSession *
SPEECH_SYNTHESIS_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, speech_synthesis_session_get_type (),
                                     SpeechSynthesisSession);
}

G_GNUC_UNUSED static inline gboolean
IS_SPEECH_SYNTHESIS_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, speech_synthesis_session_get_type ());
}

static void
speech_synthesis_session_init (SpeechSynthesisSession *session)
{
}

void
speech_synthesis_session_new (GDBusMethodInvocation *invocation,
                              GVariant *options,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  GDBusConnection *connection =
      g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  XdpAppInfo *app_info =
      xdp_invocation_ensure_app_info_sync (invocation, NULL, NULL);

  g_async_initable_new_async (
      speech_synthesis_session_get_type (), G_PRIORITY_DEFAULT, cancellable,
      callback, user_data, "sender", sender, "app-id",
      xdp_app_info_get_id (app_info), "token", lookup_session_token (options),
      "connection", connection, NULL);
}

SpeechSynthesisSession *
speech_synthesis_session_new_finish (GAsyncResult *result, GError **error)
{
  GObject *object;
  g_autoptr (GObject) source_object = g_async_result_get_source_object (result);
  g_assert (source_object != NULL);

  g_return_val_if_fail (G_IS_ASYNC_INITABLE (source_object), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  object = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
                                        result, error);

  if (object && g_initable_init (G_INITABLE (object), NULL, error))
    {
      return SPEECH_SYNTHESIS_SESSION (object);
    }

  return NULL;
}

static void
speech_synthesis_session_close (XdpSession *session)
{
  g_debug ("speech_synthesis session '%s' closed", session->id);
}

static void
speech_synthesis_session_finalize (GObject *object)
{
  SpeechSynthesisSession *synth_session = SPEECH_SYNTHESIS_SESSION (object);

  g_clear_object (&synth_session->registry);

  if (synth_session->providers != NULL)
    {
      g_ptr_array_unref (synth_session->providers);
      synth_session->providers = NULL;
    }

  G_OBJECT_CLASS (speech_synthesis_session_parent_class)->finalize (object);
}

gboolean
_provider_has_this_voices_list (SpielProvider *provider, GListModel *voices)
{
  return spiel_provider_get_voices (provider) == voices;
}

static void
handle_voices_changed (GListModel *voices,
                       guint position,
                       guint removed,
                       guint added,
                       SpeechSynthesisSession *synth_session)
{
  XdpSession *session = XDP_SESSION (synth_session);
  g_autoptr (GError) error = NULL;
  guint provider_index = 0;

  if (!synth_session->providers ||
      !g_ptr_array_find_with_equal_func (
          synth_session->providers, voices,
          (GEqualFunc) _provider_has_this_voices_list, &provider_index))
    {
      return;
    }

  const char *provider_id = spiel_provider_get_identifier (
      g_ptr_array_index (synth_session->providers, provider_index));

  if (!g_dbus_connection_emit_signal (
          session->connection, session->sender,
          "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.SpeechSynthesis", "VoicesChanged",
          g_variant_new ("(os)", session->id, provider_id), &error))
    {
      g_warning ("Failed to emit VoicesChanged signal: %s", error->message);
    }
}

static void
handle_providers_changed (GListModel *providers,
                          guint position,
                          guint removed,
                          guint added,
                          SpeechSynthesisSession *synth_session)
{
  XdpSession *session = XDP_SESSION (synth_session);
  g_autoptr (GError) error = NULL;
  gboolean emit_dbus_signal = synth_session->providers != NULL;
  guint providers_count = g_list_model_get_n_items (providers);

  if (synth_session->providers != NULL)
    {
      for (guint i = position; i < position + removed; i++)
        {
          SpielProvider *provider = NULL;
          GListModel *voices = NULL;
          if (i >= synth_session->providers->len)
            {
              g_warning (
                  "Unexpectedly out of bounds for cached speech providers");
              break;
            }
          provider = g_ptr_array_index (synth_session->providers, i);
          voices = spiel_provider_get_voices (provider);
          g_signal_handlers_disconnect_by_func (
              voices, G_CALLBACK (handle_voices_changed), synth_session);
        }

      g_ptr_array_unref (synth_session->providers);
    }

  synth_session->providers =
      g_ptr_array_new_full (providers_count, (GDestroyNotify) g_object_unref);

  for (guint i = 0; i < providers_count; i++)
    {
      SpielProvider *provider =
          SPIEL_PROVIDER (g_list_model_get_object (providers, i));
      g_ptr_array_add (synth_session->providers, g_object_ref (provider));
      if (i >= position && i < position + added)
        {
          GListModel *voices = spiel_provider_get_voices (provider);
          g_signal_connect (voices, "items-changed",
                            G_CALLBACK (handle_voices_changed), synth_session);
        }
    }

  if (emit_dbus_signal &&
      !g_dbus_connection_emit_signal (
          session->connection, session->sender,
          "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.SpeechSynthesis", "ProvidersChanged",
          g_variant_new ("(o)", session->id), &error))
    {
      g_warning ("Failed to emit ProvidersChanged signal: %s", error->message);
    }
}
static void
_on_registry_get (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  SpeechSynthesisSession *self = g_task_get_task_data (task);
  GError *error = NULL;
  GListModel *providers = NULL;

  self->registry = spiel_registry_get_finish (result, &error);
  providers = spiel_registry_get_providers (self->registry);

  g_signal_connect (providers, "items-changed",
                    G_CALLBACK (handle_providers_changed), self);

  handle_providers_changed (providers, 0, 0,
                            g_list_model_get_n_items (providers), self);

  if (error != NULL)
    {
      g_task_return_error (task, error);
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }

  g_object_unref (task);
}

static void
speech_synthesis_session_async_initable_init_async (
    GAsyncInitable *initable,
    gint io_priority,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *task = g_task_new (initable, cancellable, callback, user_data);
  SpeechSynthesisSession *self = SPEECH_SYNTHESIS_SESSION (initable);

  g_task_set_task_data (task, g_object_ref (self), g_object_unref);
  spiel_registry_get (NULL, _on_registry_get, task);
}

static gboolean
speech_synthesis_session_async_initable_init_finish (GAsyncInitable *initable,
                                                     GAsyncResult *res,
                                                     GError **error)
{
  g_return_val_if_fail (g_task_is_valid (res, initable), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
speech_synthesis_session_class_init (SpeechSynthesisSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpSessionClass *session_class = (XdpSessionClass *) klass;

  object_class->finalize = speech_synthesis_session_finalize;

  session_class->close = speech_synthesis_session_close;
}

static void
speech_synthesis_session_async_initable_iface_init (
    GAsyncInitableIface *async_initable_iface)
{
  async_initable_iface->init_async =
      speech_synthesis_session_async_initable_init_async;
  async_initable_iface->init_finish =
      speech_synthesis_session_async_initable_init_finish;
}

/*** Spiel integration ***/

/*** SpeechSynthesis boilerplate ***/

typedef struct
{
  XdpDbusSpeechSynthesisSkeleton parent_instance;
} SpeechSynthesis;

typedef struct
{
  XdpDbusSpeechSynthesisSkeletonClass parent_class;
} SpeechSynthesisClass;

static SpeechSynthesis *speech_synthesis;
static XdpDbusImplAccess *access_impl;

GType speech_synthesis_get_type (void) G_GNUC_CONST;
static void speech_synthesis_iface_init (XdpDbusSpeechSynthesisIface *iface);

G_DEFINE_TYPE_WITH_CODE (SpeechSynthesis,
                         speech_synthesis,
                         XDP_DBUS_TYPE_SPEECH_SYNTHESIS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH_SYNTHESIS,
                                                speech_synthesis_iface_init))

/*** CreateSession ***/

static void
_on_session_new (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  SpeechSynthesisSession *synth_session =
      speech_synthesis_session_new_finish (result, &error);
  GDBusMethodInvocation *invocation = user_data;
  XdpDbusSpeechSynthesis *synth_portal =
      g_object_get_data (G_OBJECT (invocation), "synth-portal");
  XdpSession *session;

  if (!synth_session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  session = XDP_SESSION (synth_session);

  if (!xdp_session_export (session, &error))
    {
      g_warning ("Failed to export session: %s", error->message);
      xdp_session_close (session, FALSE);
    }
  else
    {
      g_debug ("CreateSession new session '%s'", session->id);
      xdp_session_register (session);
    }

  xdp_dbus_speech_synthesis_complete_create_session (synth_portal, invocation,
                                                     session->id);
}

static gboolean
handle_create_session (XdpDbusSpeechSynthesis *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{

  // XXX: Do we need a lockdown option?

  g_object_set_data (G_OBJECT (invocation), "synth-portal", object);
  speech_synthesis_session_new (invocation, arg_options, NULL, _on_session_new,
                                invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/*** GetVoices ***/

static GVariant *
build_providers_variant_list (GPtrArray *providers)
{
  g_auto (GVariantBuilder) providers_list_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(ss)"));

  for (guint i = 0; i < providers->len; i++)
    {
      SpielProvider *provider = g_ptr_array_index (providers, i);
      g_variant_builder_add (&providers_list_builder, "(ss)",
                             spiel_provider_get_identifier (provider),
                             spiel_provider_get_name (provider));
    }

  return g_variant_builder_end (&providers_list_builder);
}

static void
get_permissions_in_thread_func (GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable)
{
  XdpRequest *request = task_data;
  const char *app_id = xdp_app_info_get_id (request->app_info);
  XdpPermission permission =
      xdp_get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);

  // XXX: No dialog, anything other than an explicit no is a "yes".
  if (permission == XDP_PERMISSION_NO)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No permission for speech synthesis");
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }
}

static void
handle_get_providers_cb (GObject *source_object,
                         GAsyncResult *result,
                         gpointer user_data)
{
  GDBusMethodInvocation *invocation;
  XdpRequest *request = g_task_get_task_data (G_TASK (result));
  XdpSession *session =
      g_object_get_qdata (G_OBJECT (request), quark_request_session);
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
  g_auto (GVariantBuilder) request_data_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  if (!request->exported)
    {
      return;
    }

  g_assert (g_task_is_valid (result, source_object));

  invocation = g_task_get_task_data (G_TASK (result));
  g_assert (invocation != NULL);

  if (g_task_propagate_boolean (G_TASK (result), NULL))
    {
      // Don't really care about the error so we don't capture it.
      SpeechSynthesisSession *synth_session =
          SPEECH_SYNTHESIS_SESSION (session);
      g_variant_builder_add (
          &request_data_builder, "{sv}", "providers",
          build_providers_variant_list (synth_session->providers));
      response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
    }

  g_object_set_qdata (G_OBJECT (request), quark_request_session, NULL);

  xdp_dbus_request_emit_response (
      XDP_DBUS_REQUEST (request), response,
      g_variant_builder_end (&request_data_builder));
  xdp_request_unexport (request);
}

static gboolean
handle_get_providers (XdpDbusSpeechSynthesis *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_session_handle,
                      const char *arg_parent_window,
                      GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session = xdp_session_from_request (arg_session_handle, request);
  g_autoptr (XdpDbusImplRequest) impl_request = NULL;
  g_autoptr (GTask) task = NULL;

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  // XXX: Do I need this?
  SESSION_AUTOLOCK_UNREF (session);

  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_speech_synthesis_complete_get_providers (object, invocation,
                                                    request->id);

  g_object_set_qdata_full (G_OBJECT (request), quark_request_session,
                           g_object_ref (session), g_object_unref);

  task = g_task_new (object, NULL, handle_get_providers_cb, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, get_permissions_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

gboolean
_provider_has_this_identifier (SpielProvider *provider, const char *provider_id)
{
  return g_strcmp0 (spiel_provider_get_identifier (provider), provider_id) == 0;
}

static SpielProvider *
get_provider_from_identifier (SpeechSynthesisSession *synth_session,
                              const char *provider_id)
{
  guint provider_index = 0;

  if (g_ptr_array_find_with_equal_func (
          synth_session->providers, provider_id,
          (GEqualFunc) _provider_has_this_identifier, &provider_index))
    {
      return g_ptr_array_index (synth_session->providers, provider_index);
    }

  return NULL;
}

static void
handle_get_voices_cb (GObject *source_object,
                      GAsyncResult *result,
                      gpointer user_data)
{
  GDBusMethodInvocation *invocation;
  XdpRequest *request = g_task_get_task_data (G_TASK (result));
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
  g_auto (GVariantBuilder) request_data_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!request->exported)
    {
      return;
    }

  g_assert (g_task_is_valid (result, source_object));

  invocation = g_task_get_task_data (G_TASK (result));
  g_assert (invocation != NULL);

  if (g_task_propagate_boolean (G_TASK (result), NULL))
    {
      // Don't really care about the error so we don't capture it.
      SpielProvider *provider =
          g_object_get_data (G_OBJECT (request), "provider");
      if (provider)
        {
          SpielProviderProxy *provider_proxy =
              spiel_provider_get_proxy (provider);
          GVariant *voices = spiel_provider_proxy_get_voices (provider_proxy);
          g_assert_cmpstr (g_variant_get_type_string (voices), ==, "a(ssstas)");
          g_variant_builder_add (&request_data_builder, "{sv}", "voices",
                                 voices);
          response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
        }
      else
        {
          // No provider found
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        }
    }

  xdp_dbus_request_emit_response (
      XDP_DBUS_REQUEST (request), response,
      g_variant_builder_end (&request_data_builder));
  xdp_request_unexport (request);
}

static gboolean
handle_get_voices (XdpDbusSpeechSynthesis *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_session_handle,
                   const char *arg_parent_window,
                   const char *arg_provider_id,
                   GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session = xdp_session_from_request (arg_session_handle, request);
  SpeechSynthesisSession *synth_session = NULL;
  SpielProvider *provider = NULL;
  g_autoptr (GTask) task = NULL;

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  // XXX: Do I need this?
  SESSION_AUTOLOCK_UNREF (session);

  synth_session = SPEECH_SYNTHESIS_SESSION (session);

  provider = get_provider_from_identifier (synth_session, arg_provider_id);
  if (provider)
    {
      g_object_set_data_full (G_OBJECT (request), "provider",
                              g_object_ref (provider), g_object_unref);
    }

  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_speech_synthesis_complete_get_voices (object, invocation,
                                                 request->id);

  task = g_task_new (object, NULL, handle_get_voices_cb, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, get_permissions_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
_call_synthesize_done (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  SpielProviderProxy *provider_proxy = SPIEL_PROVIDER_PROXY (source_object);
  XdpRequest *request = user_data;
  g_autoptr (GError) err = NULL;
  g_auto (GVariantBuilder) request_data_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  // XXX: Handle error
  spiel_provider_proxy_call_synthesize_finish (provider_proxy, NULL, res, &err);

  xdp_dbus_request_emit_response (
      XDP_DBUS_REQUEST (request), 0,
      g_variant_builder_end (&request_data_builder));
  xdp_request_unexport (request);
}

typedef struct
{
  GUnixFDList *fd_list;
  char *session_handle;
  char *parent_window;
  SpielProvider *provider;
  GVariant *pipe_fd;
  char *text;
  char *voice_id;
  gdouble pitch;
  gdouble rate;
  gboolean is_ssml;
  char *language;
} SynthesizeArgs;

static SynthesizeArgs *
synthesize_args_new (GUnixFDList *fd_list,
                     const gchar *session_handle,
                     const gchar *parent_window,
                     SpielProvider *provider,
                     GVariant *pipe_fd,
                     const gchar *text,
                     const gchar *voice_id,
                     gdouble pitch,
                     gdouble rate,
                     gboolean is_ssml,
                     const gchar *language)
{
  SynthesizeArgs *synth_args;

  synth_args = g_slice_new0 (SynthesizeArgs);

  if (provider)
    {
      synth_args->provider = g_object_ref (provider);
    }

  synth_args->fd_list = g_object_ref (fd_list);
  synth_args->session_handle = g_strdup (session_handle);
  synth_args->parent_window = g_strdup (parent_window);
  synth_args->pipe_fd = g_variant_ref (pipe_fd);
  synth_args->text = g_strdup (text);
  synth_args->voice_id = g_strdup (voice_id);
  synth_args->pitch = pitch;
  synth_args->rate = rate;
  synth_args->is_ssml = is_ssml;
  synth_args->language = g_strdup (language);

  return synth_args;
}

static void
synthesize_args_free (gpointer data)
{
  SynthesizeArgs *synth_args = data;
  if (synth_args == NULL)
    return;

  g_clear_object (&synth_args->fd_list);
  g_clear_pointer (&synth_args->session_handle, g_free);
  g_clear_pointer (&synth_args->parent_window, g_free);
  g_clear_object (&synth_args->provider);
  g_clear_pointer (&synth_args->pipe_fd, g_variant_unref);
  g_clear_pointer (&synth_args->text, g_free);
  g_clear_pointer (&synth_args->voice_id, g_free);
  g_clear_pointer (&synth_args->language, g_free);

  g_slice_free (SynthesizeArgs, synth_args);
}

static void
handle_synthesize_cb (GObject *source_object,
                      GAsyncResult *result,
                      gpointer user_data)
{
  GDBusMethodInvocation *invocation;
  XdpRequest *request = g_task_get_task_data (G_TASK (result));
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
  SynthesizeArgs *synth_args =
      g_object_get_data (G_OBJECT (request), "synth-args");

  if (!request->exported)
    {

      return;
    }

  g_assert (g_task_is_valid (result, source_object));

  invocation = g_task_get_task_data (G_TASK (result));
  g_assert (invocation != NULL);

  if (!g_task_propagate_boolean (G_TASK (result), NULL))
    {
      // Don't really care about the error so we don't capture it.
      response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
    }
  else if (!synth_args->provider)
    {
      // No provider found
      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
    }

  if (response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    {
      // Emit response early
      g_auto (GVariantBuilder) request_data_builder =
          G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      xdp_dbus_request_emit_response (
          XDP_DBUS_REQUEST (request), response,
          g_variant_builder_end (&request_data_builder));
      xdp_request_unexport (request);
    }
  else
    {
      SpielProviderProxy *provider_proxy =
          spiel_provider_get_proxy (synth_args->provider);
      spiel_provider_proxy_call_synthesize (
          provider_proxy, synth_args->pipe_fd, synth_args->text,
          synth_args->voice_id, synth_args->pitch, synth_args->rate,
          synth_args->is_ssml, synth_args->language, G_DBUS_CALL_FLAGS_NONE, -1,
          synth_args->fd_list, NULL, _call_synthesize_done, request);
    }
}

static gboolean
handle_synthesize (XdpDbusSpeechSynthesis *object,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList *fd_list,
                   const gchar *arg_session_handle,
                   const gchar *arg_parent_window,
                   const gchar *arg_provider_id,
                   GVariant *arg_pipe_fd,
                   const gchar *arg_text,
                   const gchar *arg_voice_id,
                   gdouble arg_pitch,
                   gdouble arg_rate,
                   gboolean arg_is_ssml,
                   const gchar *arg_language,
                   GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session = xdp_session_from_request (arg_session_handle, request);
  SpeechSynthesisSession *synth_session = NULL;
  SpielProvider *provider = NULL;
  SynthesizeArgs *synth_args = NULL;
  g_autoptr (GTask) task = NULL;

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  // XXX: Do I need this?
  SESSION_AUTOLOCK_UNREF (session);

  synth_session = SPEECH_SYNTHESIS_SESSION (session);

  provider = get_provider_from_identifier (synth_session, arg_provider_id);

  synth_args = synthesize_args_new (
      fd_list, arg_session_handle, arg_parent_window, provider, arg_pipe_fd,
      arg_text, arg_voice_id, arg_pitch, arg_rate, arg_is_ssml, arg_language);

  g_object_set_data_full (G_OBJECT (request), "synth-args", synth_args,
                          synthesize_args_free);

  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_speech_synthesis_complete_synthesize (object, invocation, fd_list,
                                                 request->id);

  task = g_task_new (object, NULL, handle_synthesize_cb, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, get_permissions_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/************/

static void
speech_synthesis_iface_init (XdpDbusSpeechSynthesisIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_get_providers = handle_get_providers;
  iface->handle_get_voices = handle_get_voices;
  iface->handle_synthesize = handle_synthesize;
}

static void
speech_synthesis_init (SpeechSynthesis *speech_synthesis)
{
  xdp_dbus_speech_synthesis_set_version (
      XDP_DBUS_SPEECH_SYNTHESIS (speech_synthesis), 1);
}

static void
speech_synthesis_class_init (SpeechSynthesisClass *klass)
{
  quark_request_session =
      g_quark_from_static_string ("-xdp-request-speech_synthesis-session");
}

GDBusInterfaceSkeleton *
speech_synthesis_create (GDBusConnection *connection, const char *dbus_name)
{
  g_autoptr (GError) error = NULL;

  access_impl = xdp_dbus_impl_access_proxy_new_sync (
      connection, G_DBUS_PROXY_FLAGS_NONE, dbus_name,
      DESKTOP_PORTAL_OBJECT_PATH, NULL, &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  speech_synthesis = g_object_new (speech_synthesis_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (speech_synthesis);
}
