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

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib-unix.h>

#include "speech-provider-dbus.h"

#include "speech.h"
#include "xdp-dbus.h"
#include "xdp-permissions.h"
#include "xdp-request.h"
#include "xdp-session.h"
#include "xdp-utils.h"
#include "xdp-context.h"

#define PERMISSION_TABLE "speech"
#define PERMISSION_ID "speech"

#define PROVIDER_SUFFIX ".Speech.Provider"

static GQuark quark_request_session;
static GQuark quark_provider_installed;

typedef struct
{
  XdpSession parent;

  guint subscription_ids[2];
  GHashTable *providers;
} SpeechSession;

typedef struct
{
  XdpSessionClass parent_class;
} SpeechSessionClass;

GType speech_session_get_type (void);

static void speech_session_async_initable_iface_init (
    GAsyncInitableIface *async_initable_iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
    SpeechSession,
    speech_session,
    xdp_session_get_type (),
    G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                           speech_session_async_initable_iface_init))

G_GNUC_UNUSED static inline SpeechSession *
SPEECH_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, speech_session_get_type (),
                                     SpeechSession);
}

G_GNUC_UNUSED static inline gboolean
IS_SPEECH_SESSION (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, speech_session_get_type ());
}

static void
speech_session_init (SpeechSession *session)
{
  quark_provider_installed =
      g_quark_from_static_string ("-xdp-speech-provider-installed");
}

static void
speech_session_new (GDBusMethodInvocation *invocation,
                    GVariant *options,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  GDBusConnection *connection =
      g_dbus_method_invocation_get_connection (invocation);

  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);

  g_async_initable_new_async (
      speech_session_get_type (),
      G_PRIORITY_DEFAULT, cancellable, callback, user_data,
      "sender", sender,
      "app-id", xdp_app_info_get_id (app_info),
      "token", lookup_session_token (options),
      "connection", connection, NULL);
}

static SpeechSession *
speech_session_new_finish (GAsyncResult *result, GError **error)
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
      return SPEECH_SESSION (object);
    }

  return NULL;
}

static void
speech_session_close (XdpSession *session)
{
}

static void
speech_session_finalize (GObject *object)
{
  SpeechSession *synth_session = SPEECH_SESSION (object);

  if (synth_session->subscription_ids[0])
    {
      g_dbus_connection_signal_unsubscribe (
          XDP_SESSION (synth_session)->connection,
          synth_session->subscription_ids[0]);
      synth_session->subscription_ids[0] = 0;
    }

  if (synth_session->subscription_ids[1])
    {
      g_dbus_connection_signal_unsubscribe (
          XDP_SESSION (synth_session)->connection,
          synth_session->subscription_ids[1]);
      synth_session->subscription_ids[1] = 0;
    }

  if (synth_session->providers != NULL)
    {
      g_hash_table_unref (synth_session->providers);
      synth_session->providers = NULL;
    }

  G_OBJECT_CLASS (speech_session_parent_class)->finalize (object);
}

static gboolean
handle_voices_changed (SpeechProviderProxy *provider_proxy,
                       GParamSpec *spec,
                       SpeechSession *synth_session)
{
  XdpSession *session = XDP_SESSION (synth_session);
  g_autoptr (GError) error = NULL;
  g_autofree char *name_owner =
      g_dbus_proxy_get_name_owner (G_DBUS_PROXY (provider_proxy));
  const char *provider_id =
      g_dbus_proxy_get_name (G_DBUS_PROXY (provider_proxy));

  if (name_owner == NULL)
    {
      // Got a change notification because a service left the bus.
      return TRUE;
    }

  if (!synth_session->providers ||
      !g_hash_table_contains (synth_session->providers, provider_id))
    {
      return TRUE;
    }

  if (!g_dbus_connection_emit_signal (
          session->connection, session->sender,
          "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.Speech", "VoicesChanged",
          g_variant_new ("(os)", session->id, provider_id), &error))
    {
      g_warning ("Failed to emit VoicesChanged signal: %s", error->message);
    }

  return TRUE;
}

static void
collect_providers_in_thread_func (GTask *task,
                                  gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
  XdpSession *session = XDP_SESSION (source_object);
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) providers = g_hash_table_new_full (
      g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_object_unref);
  const char *list_name_methods[] = { "ListActivatableNames", "ListNames",
                                      NULL };
  for (const char **method = list_name_methods; *method; method++)
    {
      char *service_name = NULL;
      g_autoptr (GVariantIter) iter = NULL;
      g_autoptr (GVariant) ret = g_dbus_connection_call_sync (
          session->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", *method, NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
          -1, cancellable, &error);
      if (error)
        {
          g_warning ("Error calling list (%s): %s\n", *method, error->message);
          g_task_return_error (task, error);
          return;
        }

      g_variant_get (ret, "(as)", &iter);
      while (g_variant_iter_next (iter, "&s", &service_name))
        {
          SpeechProviderProxy *provider_proxy = NULL;
          if (!g_str_has_suffix (service_name, PROVIDER_SUFFIX))
            {
              continue;
            }

          provider_proxy = g_hash_table_lookup (providers, service_name);

          if (!provider_proxy)
            {
              g_auto (GStrv) split_name = g_strsplit (service_name, ".", 0);
              g_autofree char *partial_path = g_strjoinv ("/", split_name);
              g_autofree char *obj_path = g_strdup_printf ("/%s", partial_path);

              provider_proxy = speech_provider_proxy_proxy_new_sync (
                  session->connection, 0, service_name, obj_path, cancellable,
                  &error);

              if (error)
                {
                  g_warning ("Error creating proxy for '%s': %s\n",
                             service_name, error->message);
                  continue;
                }
              g_hash_table_insert (providers, g_strdup (service_name),
                                   provider_proxy);
            }

          if (g_str_equal (*method, "ListActivatableNames"))
            {
              g_object_set_qdata (G_OBJECT (provider_proxy),
                                  quark_provider_installed,
                                  GUINT_TO_POINTER (1));
            }
        }
    }

  g_task_return_pointer (task, g_steal_pointer (&providers), g_object_unref);
}

static void
handle_providers_changed_cb (GObject *source_object,
                             GAsyncResult *result,
                             gpointer user_data)
{
  XdpSession *session = XDP_SESSION (source_object);
  SpeechSession *self = SPEECH_SESSION (source_object);
  g_autoptr (GError) error = NULL;
  GHashTableIter providers_iter;
  SpeechProviderProxy *provider_proxy;
  char *provider_id = NULL;
  gboolean changed = FALSE;
  g_autoptr (GHashTable) providers =
      g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_warning ("Failed to collect providers in change callback: %s",
                 error->message);
      return;
    }

  g_hash_table_iter_init (&providers_iter, self->providers);
  while (g_hash_table_iter_next (&providers_iter, (gpointer *) &provider_id,
                                 (gpointer *) &provider_proxy))
    {
      SpeechProviderProxy *new_provider_proxy =
          g_hash_table_lookup (providers, provider_id);
      if (new_provider_proxy)
        {
          // Copy installed flag from new proxy instance
          g_object_set_qdata (G_OBJECT (provider_proxy),
                              quark_provider_installed,
                              g_object_get_qdata (G_OBJECT (new_provider_proxy),
                                                  quark_provider_installed));
        }
      else
        {
          // Disconnect signal and remove cached provider.
          changed = TRUE;
          g_signal_handlers_disconnect_by_func (provider_proxy,
                                                handle_voices_changed, self);
          g_hash_table_iter_remove (&providers_iter);
        }
    }

  g_hash_table_iter_init (&providers_iter, providers);
  while (g_hash_table_iter_next (&providers_iter, (gpointer *) &provider_id,
                                 (gpointer *) &provider_proxy))
    {

      if (!g_hash_table_contains (self->providers, provider_id))
        {
          changed = TRUE;
          g_hash_table_insert (self->providers, provider_id, provider_proxy);
          g_signal_connect (provider_proxy, "notify::voices",
                            G_CALLBACK (handle_voices_changed), self);
          g_hash_table_iter_steal (&providers_iter);
        }
    }

  if (!changed)
    {
      return;
    }

  if (!g_dbus_connection_emit_signal (
          session->connection, session->sender,
          "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.Speech", "ProvidersChanged",
          g_variant_new ("(o)", session->id), &error))
    {
      g_warning ("Failed to emit ProvidersChanged signal: %s", error->message);
    }

  g_debug ("ProvidersChanged signal handled for speech session");
}

static void
handle_providers_changed (GDBusConnection *connection,
                          const gchar *sender_name,
                          const gchar *object_path,
                          const gchar *interface_name,
                          const gchar *signal_name,
                          GVariant *parameters,
                          gpointer user_data)
{
  XdpSession *session = XDP_SESSION (user_data);
  GTask *task = g_task_new (session, NULL, handle_providers_changed_cb, NULL);

  SESSION_AUTOLOCK_UNREF (session);

  g_task_run_in_thread (task, collect_providers_in_thread_func);
}

static void
speech_session_async_initable_init_async (
    GAsyncInitable *initable,
    gint io_priority,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *task = g_task_new (initable, cancellable, callback, user_data);
  SpeechSession *self = SPEECH_SESSION (initable);

  g_task_set_task_data (task, g_object_ref (self), g_object_unref);
  g_task_run_in_thread (task, collect_providers_in_thread_func);
}

static gboolean
speech_session_async_initable_init_finish (GAsyncInitable *initable,
                                           GAsyncResult *res,
                                           GError **error)
{
  SpeechSession *self = SPEECH_SESSION (initable);
  XdpSession *session = XDP_SESSION (initable);
  GHashTableIter providers_iter;
  SpeechProviderProxy *provider_proxy;

  g_return_val_if_fail (g_task_is_valid (res, initable), FALSE);

  self->providers = g_task_propagate_pointer (G_TASK (res), error);
  g_return_val_if_fail (self->providers != NULL, FALSE);

  g_hash_table_iter_init (&providers_iter, self->providers);
  while (g_hash_table_iter_next (&providers_iter, NULL,
                                 (gpointer *) &provider_proxy))
    {
      g_signal_connect (provider_proxy, "notify::voices",
                        G_CALLBACK (handle_voices_changed), self);
    }

  self->subscription_ids[0] = g_dbus_connection_signal_subscribe (
      session->connection, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "ActivatableServicesChanged", "/org/freedesktop/DBus", NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, handle_providers_changed, self, NULL);

  self->subscription_ids[1] = g_dbus_connection_signal_subscribe (
      session->connection, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, handle_providers_changed, g_object_ref (self),
      g_object_unref);

  return TRUE;
}

static void
speech_session_class_init (SpeechSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpSessionClass *session_class = (XdpSessionClass *) klass;

  object_class->finalize = speech_session_finalize;

  session_class->close = speech_session_close;
}

static void
speech_session_async_initable_iface_init (
    GAsyncInitableIface *async_initable_iface)
{
  async_initable_iface->init_async =
      speech_session_async_initable_init_async;
  async_initable_iface->init_finish =
      speech_session_async_initable_init_finish;
}

/*** Spiel integration ***/

/*** Speech boilerplate ***/

typedef struct
{
  XdpDbusSpeechSkeleton parent_instance;
} Speech;

typedef struct
{
  XdpDbusSpeechSkeletonClass parent_class;
} SpeechClass;

GType speech_get_type (void) G_GNUC_CONST;
static void speech_iface_init (XdpDbusSpeechIface *iface);

G_DEFINE_TYPE_WITH_CODE (Speech,
                         speech,
                         XDP_DBUS_TYPE_SPEECH_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH,
                                                speech_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Speech, g_object_unref)

/*** CreateSession ***/

static void
_on_session_new (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  SpeechSession *synth_session =
      speech_session_new_finish (result, &error);
  GDBusMethodInvocation *invocation = user_data;
  XdpDbusSpeech *synth_portal =
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

  xdp_dbus_speech_complete_create_session (synth_portal, invocation,
                                           session->id);
}

static gboolean
handle_create_session (XdpDbusSpeech *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{

  // XXX: Do we need a lockdown option?

  g_object_set_data (G_OBJECT (invocation), "synth-portal", object);
  speech_session_new (invocation, arg_options, NULL, _on_session_new,
                      invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/*** GetVoices ***/

static GVariant *
build_providers_variant_list (GHashTable *providers)
{
  GHashTableIter providers_iter;
  SpeechProviderProxy *provider_proxy = NULL;
  g_auto (GVariantBuilder) providers_list_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(ss)"));

  g_hash_table_iter_init (&providers_iter, providers);
  while (g_hash_table_iter_next (&providers_iter, NULL,
                                 (gpointer *) &provider_proxy))
    {
      g_variant_builder_add (
          &providers_list_builder, "(ss)",
          g_dbus_proxy_get_name (G_DBUS_PROXY (provider_proxy)),
          speech_provider_proxy_get_name (provider_proxy));
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
  XdpPermission permission =
      xdp_get_permission_sync (request->app_info, PERMISSION_TABLE, PERMISSION_ID);

  // XXX: No dialog, anything other than an explicit no is a "yes".
  if (permission == XDP_PERMISSION_NO)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No permission for speech");
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }
}

/* Helper: start a permission-check task for a request. This centralises the
 * common GTask creation + task-data setup used by several handlers. */
static GTask *
start_permission_task_for_request (GObject *source_object,
                                   XdpRequest *request,
                                   GAsyncReadyCallback callback)
{
  GTask *task = g_task_new (source_object, NULL, callback, NULL);

  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, get_permissions_in_thread_func);

  return task;
}

static void
handle_get_providers_cb (GObject *source_object,
                         GAsyncResult *result,
                         gpointer user_data)
{
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

  if (g_task_propagate_boolean (G_TASK (result), NULL))
    {
      // Don't really care about the error so we don't capture it.
      SpeechSession *synth_session =
          SPEECH_SESSION (session);
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
handle_get_providers (XdpDbusSpeech *object,
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

  g_object_set_qdata_full (G_OBJECT (request), quark_request_session,
                           g_object_ref (session), g_object_unref);

  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_speech_complete_get_providers (object, invocation,
                                          request->id);

  task = start_permission_task_for_request (G_OBJECT (object), request,
                                            handle_get_providers_cb);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
handle_get_voices_cb (GObject *source_object,
                      GAsyncResult *result,
                      gpointer user_data)
{
  XdpRequest *request = g_task_get_task_data (G_TASK (result));
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
  g_auto (GVariantBuilder) request_data_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  if (!request->exported)
    {
      return;
    }

  g_assert (g_task_is_valid (result, source_object));

  if (g_task_propagate_boolean (G_TASK (result), NULL))
    {
      // Don't really care about the error so we don't capture it.
      SpeechProviderProxy *provider_proxy =
          g_object_get_data (G_OBJECT (request), "provider");
      if (provider_proxy)
        {
          GVariant *voices = speech_provider_proxy_get_voices (provider_proxy);
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
handle_get_voices (XdpDbusSpeech *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_session_handle,
                   const char *arg_parent_window,
                   const char *arg_provider_id,
                   GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session = xdp_session_from_request (arg_session_handle, request);
  SpeechSession *synth_session = NULL;
  SpeechProviderProxy *provider = NULL;
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

  synth_session = SPEECH_SESSION (session);

  provider = g_hash_table_lookup (synth_session->providers, arg_provider_id);
  if (provider)
    {
      g_object_set_data_full (G_OBJECT (request), "provider",
                              g_object_ref (provider), g_object_unref);
    }

  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_speech_complete_get_voices (object, invocation,
                                       request->id);

  task = start_permission_task_for_request (G_OBJECT (object), request,
                                            handle_get_voices_cb);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
_call_synthesize_done (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  SpeechProviderProxy *provider_proxy = SPEECH_PROVIDER_PROXY (source_object);
  XdpRequest *request = user_data;
  g_autoptr (GError) err = NULL;
  g_auto (GVariantBuilder) request_data_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  /* Finish the synthesize call. Any provider errors are logged but will not
   * block emitting a portal response here. */
  speech_provider_proxy_call_synthesize_finish (provider_proxy, NULL, res,
                                                &err);
  if (err)
    g_warning ("speech provider synthesize failed: %s", err->message);

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
  SpeechProviderProxy *provider_proxy;
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
                     SpeechProviderProxy *provider_proxy,
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

  if (provider_proxy)
    {
      synth_args->provider_proxy = g_object_ref (provider_proxy);
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
  g_clear_object (&synth_args->provider_proxy);
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
  XdpRequest *request = g_task_get_task_data (G_TASK (result));
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
  SynthesizeArgs *synth_args =
      g_object_get_data (G_OBJECT (request), "synth-args");

  if (!request->exported)
    {

      return;
    }

  g_assert (g_task_is_valid (result, source_object));

  if (!g_task_propagate_boolean (G_TASK (result), NULL))
    {
      // Don't really care about the error so we don't capture it.
      response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
    }
  else if (!synth_args->provider_proxy)
    {
      // No provider found
      g_warning ("No provider found");
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
      speech_provider_proxy_call_synthesize (
          synth_args->provider_proxy, synth_args->pipe_fd, synth_args->text,
          synth_args->voice_id, synth_args->pitch, synth_args->rate,
          synth_args->is_ssml, synth_args->language, G_DBUS_CALL_FLAGS_NONE, -1,
          synth_args->fd_list, NULL, _call_synthesize_done, request);
    }
}

static gboolean
handle_synthesize (XdpDbusSpeech *object,
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
  SpeechSession *synth_session = NULL;
  SpeechProviderProxy *provider_proxy = NULL;
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

  synth_session = SPEECH_SESSION (session);

  provider_proxy =
      g_hash_table_lookup (synth_session->providers, arg_provider_id);

  synth_args =
      synthesize_args_new (fd_list, arg_session_handle, arg_parent_window,
                           provider_proxy, arg_pipe_fd, arg_text, arg_voice_id,
                           arg_pitch, arg_rate, arg_is_ssml, arg_language);

  g_object_set_data_full (G_OBJECT (request), "synth-args", synth_args,
                          synthesize_args_free);

  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_speech_complete_synthesize (object, invocation, fd_list,
                                       request->id);

  task = start_permission_task_for_request (G_OBJECT (object), request,
                                            handle_synthesize_cb);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/************/

static void
speech_iface_init (XdpDbusSpeechIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_get_providers = handle_get_providers;
  iface->handle_get_voices = handle_get_voices;
  iface->handle_synthesize = handle_synthesize;
}

static void
speech_init (Speech *speech)
{
  xdp_dbus_speech_set_version (
      XDP_DBUS_SPEECH (speech), 1);
}

static void
speech_class_init (SpeechClass *klass)
{
  quark_request_session =
      g_quark_from_static_string ("-xdp-request-speech-session");
}

void
init_speech (XdpContext *context)
{
  g_autoptr (Speech) speech = NULL;

  speech = g_object_new (speech_get_type (), NULL);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&speech)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}