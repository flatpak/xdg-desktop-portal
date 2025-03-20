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

#include "speech.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#include "speech-provider-dbus.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-permissions.h"
#include "xdp-request-dex.h"
#include "xdp-session-dex.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "speech"
#define PERMISSION_ID "speech"

#define PROVIDER_SUFFIX ".Speech.Provider"

struct _XdpSpeech
{
  XdpDbusSpeechSkeleton parent_instance;
  XdpContext           *context;
  XdpDbusImplAccess    *access_impl;

  XdpSessionDexStore *sessions;
};

#define XDP_TYPE_SPEECH (xdp_speech_get_type ())
G_DECLARE_FINAL_TYPE (XdpSpeech, xdp_speech, XDP, SPEECH, XdpDbusSpeechSkeleton);

static void xdp_speech_iface_init (XdpDbusSpeechIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSpeech,
                               xdp_speech,
                               XDP_DBUS_TYPE_SPEECH_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH, xdp_speech_iface_init));

static GQuark quark_provider_voices_changed_handler;

typedef struct
{
  GObject parent;

  XdpSessionDex *session;

  gulong subscription_ids[2];

  GHashTable *providers;
} SessionWrapper;

typedef struct
{
  GObjectClass parent_class;
} SessionWrapperClass;

GType session_wrapper_get_type (void);

G_DEFINE_TYPE (SessionWrapper, session_wrapper, G_TYPE_OBJECT)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SessionWrapper, g_object_unref)

G_GNUC_UNUSED static inline SessionWrapper *
SESSION_WRAPPER (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, session_wrapper_get_type (), SessionWrapper);
}

G_GNUC_UNUSED static inline gboolean
IS_SESSION_WRAPPER (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, session_wrapper_get_type ());
}

static DexFuture *
get_allowed_providers_future (void)
{
  g_autoptr (XdpDbusImplPermissionStoreLookupResult) result = NULL;
  g_autoptr (GVariant) allowed_providers = NULL;

  result = dex_await_boxed (xdp_dbus_impl_permission_store_call_lookup_future (
                                xdp_get_permission_store (), PERMISSION_TABLE, PERMISSION_ID),
                            NULL);

  if (!result)
    {
      return dex_future_new_for_error (
          g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No speech permissions entry"));
    }

  allowed_providers = g_variant_get_variant (result->data);
  if (!allowed_providers || !g_variant_type_is_container (g_variant_get_type (allowed_providers)))
    {
      return dex_future_new_for_error (
          g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No speech providers allowed"));
    }

  return dex_future_new_take_variant (g_steal_pointer (&allowed_providers));
}

static DexFuture *
ask_speech_permissions_future (XdpSpeech *speech, XdpRequestDex *request, const char *parent_window)
{
  XdpAppInfo *app_info = xdp_request_dex_get_app_info (request);
  const char *app_name = xdp_app_info_get_app_display_name (app_info);
  g_autoptr (XdpDbusImplAccessAccessDialogResult) result = NULL;
  g_auto (GVariantBuilder) access_opt_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *title = NULL;
  g_autofree gchar *subtitle = NULL;
  const gchar *body;
  guint perms = dex_await_uint (
      xdp_permission_get_future (app_info, PERMISSION_TABLE, PERMISSION_ID), &error);

  if (error)
    {
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  if (perms == XDP_PERMISSION_YES)
    {
      return dex_future_new_true ();
    }

  if (perms == XDP_PERMISSION_NO)
    {
      return dex_future_new_for_error (
          g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No permission for speech"));
    }

  g_variant_builder_add (&access_opt_builder, "{sv}", "deny_label",
                         g_variant_new_string (_ ("Deny")));
  g_variant_builder_add (&access_opt_builder, "{sv}", "grant_label",
                         g_variant_new_string (_ ("Allow")));
  g_variant_builder_add (&access_opt_builder, "{sv}", "icon",
                         g_variant_new_string ("preferences-desktop-wallpaper-symbolic"));

  if (app_name)
    {
      title = g_strdup_printf (_ ("Allow %s to Use Speech Synthesis?"), app_name);
      subtitle =
          g_strdup_printf (_ ("%s wants to use external speech synthesis services"), app_name);
    }
  else
    {
      title = g_strdup (_ ("Allow Apps to Use Speech Synthesis?"));
      subtitle = g_strdup (_ ("An app wants to use external speech synthesis services"));
    }

  body = _ ("This permission can be changed at any time from the privacy settings");

  result = dex_await_boxed (xdp_dbus_impl_access_call_access_dialog_future (
                                speech->access_impl, xdp_request_dex_get_object_path (request),
                                xdp_app_info_get_id (app_info), parent_window, title, subtitle,
                                body, g_variant_builder_end (&access_opt_builder)),
                            &error);

  if (error)
    {
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  perms = result->response == 0 ? XDP_PERMISSION_YES : XDP_PERMISSION_NO;

  if (!dex_await_boolean (
          xdp_permission_set_future (app_info, PERMISSION_TABLE, PERMISSION_ID, perms), &error))
    g_warning ("Setting unset permission failed: %s", error->message);

  if (perms != XDP_PERMISSION_YES)
    {
      return dex_future_new_for_error (
          g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No permission for speech"));
    }

  return dex_future_new_true ();
}

static void
provider_unref (GObject *object)
{
  gulong handler_id =
      GPOINTER_TO_SIZE (g_object_get_qdata (object, quark_provider_voices_changed_handler));

  if (handler_id)
    {
      g_signal_handler_disconnect (object, handler_id);
      g_object_set_qdata (object, quark_provider_voices_changed_handler, NULL);
    }

  g_object_unref (object);
}

static DexFuture *
collect_providers (SessionWrapper *session_wrapper)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection = xdp_session_dex_get_connection (session_wrapper->session);
  g_autoptr (GHashTable) providers =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) provider_unref);
  g_autoptr (GVariant) allowed_providers = NULL;
  const char *list_name_methods[] = { "ListActivatableNames", "ListNames", NULL };
  guint perms = dex_await_uint (
      xdp_permission_get_future (xdp_session_dex_get_app_info (session_wrapper->session),
                                 PERMISSION_TABLE, PERMISSION_ID),
      NULL);

  if (perms != XDP_PERMISSION_YES)
    {
      /* If this app does not have permission, just quietly return an empty providers table
       * this way we won't emit any changed notifications either. If the permission changes, the
       * app will be notified new providers were added. */
      return dex_future_new_take_boxed (G_TYPE_HASH_TABLE, g_steal_pointer (&providers));
    }

  allowed_providers = dex_await_variant (get_allowed_providers_future (), NULL);

  /* Collect both activatable providers and running providers into a single hashtable */
  for (const char **method = list_name_methods; *method; method++)
    {
      const char *service_name = NULL;
      g_autoptr (GVariantIter) iter = NULL;
      g_autoptr (GVariant) ret = dex_await_variant (
          dex_dbus_connection_call (connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus", *method, NULL, NULL,
                                    G_DBUS_CALL_FLAGS_NONE, -1),
          &error);
      if (error)
        {
          g_warning ("Error calling list (%s): %s", *method, error->message);
          return dex_future_new_for_error (g_steal_pointer (&error));
        }

      g_variant_get (ret, "(as)", &iter);
      while (g_variant_iter_next (iter, "&s", &service_name))
        {
          SpeechProviderProxy *provider_proxy = NULL;
          gboolean allowed = FALSE;

          if (!g_str_has_suffix (service_name, PROVIDER_SUFFIX))
            {
              /* Providers are detected by their suffix */
              continue;
            }

          if (!allowed_providers ||
              !g_variant_lookup (allowed_providers, service_name, "b", &allowed) || !allowed)
            {
              /* Provider not explicitly allowed. */
              continue;
            }

          provider_proxy = g_hash_table_lookup (providers, service_name);

          if (!provider_proxy)
            {
              /* Encountering this speech provider for the first time, instantiate proxy and store
               * it in table */
              g_auto (GStrv) split_name = g_strsplit (service_name, ".", 0);
              g_autofree char *partial_path = g_strjoinv ("/", split_name);
              g_autofree char *obj_path = g_strdup_printf ("/%s", partial_path);

              provider_proxy = dex_await_object (
                  speech_provider_proxy_proxy_new_future (connection, 0, service_name, obj_path),
                  &error);

              if (error)
                {
                  g_warning ("Error creating proxy for '%s': %s", service_name, error->message);
                  continue;
                }
              g_hash_table_insert (providers, g_strdup (service_name), provider_proxy);
            }
        }
    }

  return dex_future_new_take_boxed (G_TYPE_HASH_TABLE, g_steal_pointer (&providers));
}

static void
emit_voices_changed (SessionWrapper *session_wrapper)
{
  GDBusConnection *connection = xdp_session_dex_get_connection (session_wrapper->session);
  XdpAppInfo *app_info = xdp_session_dex_get_app_info (session_wrapper->session);
  const char *sender = xdp_app_info_get_sender (app_info);

  if (!connection)
    {
      return;
    }

  g_autoptr (GError) error = NULL;
  if (!g_dbus_connection_emit_signal (
          connection, sender, "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Speech",
          "VoicesChanged",
          g_variant_new ("(o)", xdp_session_dex_get_object_path (session_wrapper->session)),
          &error))
    {
      g_warning ("Failed to emit VoicesChanged signal: %s", error->message);
    }
}

static void
voices_changed_cb (SpeechProviderProxy *provider_proxy,
                   GParamSpec          *pspec,
                   SessionWrapper      *session_wrapper)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (provider_proxy));
  const char *provider_id = g_dbus_proxy_get_name (G_DBUS_PROXY (provider_proxy));

  if (name_owner == NULL)
    {
      /* Got a change notification because a service left the bus. */
      return;
    }

  if (!session_wrapper->providers ||
      !g_hash_table_contains (session_wrapper->providers, provider_id))
    {
      g_warning ("Got a voices changed notification from a provider that is not in our table!");
      return;
    }

  emit_voices_changed (session_wrapper);
}

static DexFuture *
update_providers (SessionWrapper *session_wrapper)
{
  g_autoptr (GError) error = NULL;
  GHashTableIter providers_iter;
  SpeechProviderProxy *provider_proxy;
  char *provider_id = NULL;
  gboolean changed = FALSE;
  g_autoptr (GHashTable) providers = dex_await_boxed (collect_providers (session_wrapper), &error);

  if (error)
    {
      g_warning ("Failed to collect providers in change callback: %s", error->message);
      return dex_future_new_false ();
    }

  if (session_wrapper->providers == NULL)
    {
      /* This is being called from the constructor */
      session_wrapper->providers = g_hash_table_new_similar (providers);
    }

  /* Iterate over cached providers and remove any that are not in our new list */
  g_hash_table_iter_init (&providers_iter, session_wrapper->providers);
  while (g_hash_table_iter_next (&providers_iter, (gpointer *) &provider_id,
                                 (gpointer *) &provider_proxy))
    {
      SpeechProviderProxy *new_provider_proxy = g_hash_table_lookup (providers, provider_id);

      if (!new_provider_proxy)
        {
          /* Remove cached provider. */
          changed = TRUE;
          g_hash_table_iter_remove (&providers_iter);
        }
    }

  /* Iterate over new providers and add to out cache any that are not already there */
  g_hash_table_iter_init (&providers_iter, providers);
  while (g_hash_table_iter_next (&providers_iter, (gpointer *) &provider_id,
                                 (gpointer *) &provider_proxy))
    {
      if (!g_hash_table_contains (session_wrapper->providers, provider_id))
        {
          gulong handler_id;

          changed = TRUE;
          g_hash_table_insert (session_wrapper->providers, provider_id, provider_proxy);
          handler_id = g_signal_connect (provider_proxy, "notify::voices",
                                         G_CALLBACK (voices_changed_cb), session_wrapper);
          g_object_set_qdata (G_OBJECT (provider_proxy), quark_provider_voices_changed_handler,
                              GSIZE_TO_POINTER (handler_id));

          g_hash_table_iter_steal (&providers_iter);
        }
    }

  return dex_future_new_for_boolean (changed);
}

static DexFuture *
update_providers_and_emit_change (SessionWrapper *session_wrapper)
{
  g_autoptr (GError) error = NULL;
  if (!dex_await_boolean (update_providers (session_wrapper), &error))
    {
      if (error != NULL)
        {
          g_warning ("Failed to update providers: %s\n", error->message);
        }
      return dex_future_new_false ();
    }

  emit_voices_changed (session_wrapper);
  return dex_future_new_true ();
}

static void
providers_changed_cb (GDBusConnection *connection,
                      const char      *sender_name,
                      const char      *object_path,
                      const char      *interface_name,
                      const char      *signal_name,
                      GVariant        *parameters,
                      gpointer         user_data)
{
  SessionWrapper *session_wrapper = SESSION_WRAPPER (user_data);

  dex_future_disown (dex_scheduler_spawn (NULL, 0, (DexFiberFunc) update_providers_and_emit_change,
                                          g_object_ref (session_wrapper), g_object_unref));
}

static void
permissions_changed_cb (SessionWrapper             *session_wrapper,
                        const char                 *arg_table,
                        const char                 *arg_id,
                        gboolean                    arg_deleted,
                        GVariant                   *arg_data,
                        GVariant                   *arg_permissions,
                        XdpDbusImplPermissionStore *permission_store)
{
  if (g_strcmp0 (arg_table, PERMISSION_TABLE) != 0 || g_strcmp0 (arg_id, PERMISSION_ID) != 0)
    return;

  dex_future_disown (dex_scheduler_spawn (NULL, 0, (DexFiberFunc) update_providers_and_emit_change,
                                          g_object_ref (session_wrapper), g_object_unref));
}

static void
session_wrapper_init (SessionWrapper *session_wrapper)
{
}

static void
session_wrapper_finalize (GObject *object)
{
  SessionWrapper *session_wrapper = SESSION_WRAPPER (object);
  GDBusConnection *connection = xdp_session_dex_get_connection (session_wrapper->session);

  g_clear_pointer (&session_wrapper->providers, g_hash_table_unref);

  if (session_wrapper->subscription_ids[0])
    {
      g_dbus_connection_signal_unsubscribe (connection, session_wrapper->subscription_ids[0]);
      session_wrapper->subscription_ids[0] = 0;
    }

  if (session_wrapper->subscription_ids[1])
    {
      g_dbus_connection_signal_unsubscribe (connection, session_wrapper->subscription_ids[1]);
      session_wrapper->subscription_ids[1] = 0;
    }

  g_clear_object (&session_wrapper->session);

  G_OBJECT_CLASS (session_wrapper_parent_class)->finalize (object);
}

static void
session_wrapper_class_init (SessionWrapperClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = session_wrapper_finalize;

  quark_provider_voices_changed_handler =
      g_quark_from_static_string ("xdp-speech-provider-voices-changed-handler");
}

static DexFuture *
session_wrapper_new (XdpSessionDex *session)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection = xdp_session_dex_get_connection (session);
  g_autoptr (SessionWrapper) session_wrapper;

  session_wrapper = g_object_new (session_wrapper_get_type (), NULL);

  session_wrapper->subscription_ids[0] = g_dbus_connection_signal_subscribe (
      connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "ActivatableServicesChanged",
      "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, providers_changed_cb,
      session_wrapper, NULL);

  session_wrapper->subscription_ids[1] = g_dbus_connection_signal_subscribe (
      connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, providers_changed_cb,
      session_wrapper, NULL);

  g_signal_connect_object (xdp_get_permission_store (), "changed",
                           G_CALLBACK (permissions_changed_cb), session_wrapper, G_CONNECT_SWAPPED);

  session_wrapper->session = g_object_ref (session);

  if (!dex_await_boolean (update_providers (session_wrapper), &error) && error != NULL)
    {
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  return dex_future_new_take_object (g_steal_pointer (&session_wrapper));
}

static gboolean
handle_create_session (XdpDbusSpeech         *object,
                       GDBusMethodInvocation *invocation,
                       GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  g_autoptr (GError) error = NULL;
  g_autoptr (XdpSessionDex) session = NULL;
  g_autoptr (SessionWrapper) session_wrapper = NULL;

  session = dex_await_object (
      xdp_session_dex_new (speech->context, xdp_invocation_get_app_info (invocation),
                           G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
      &error);

  if (!session)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session_wrapper = dex_await_object (session_wrapper_new (session), &error);

  if (!session_wrapper)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_create_session (object, g_steal_pointer (&invocation),
                                           xdp_session_dex_get_object_path (session));

  xdp_session_dex_store_take_session (speech->sessions, g_steal_pointer (&session_wrapper));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_provider_details (XdpDbusSpeech         *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_session_handle,
                             const char            *arg_parent_window,
                             const char            *arg_provider_id,
                             GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  XdpRequestDex *request = NULL;
  SessionWrapper *session_wrapper = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
  g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GError) error = NULL;

  session_wrapper =
      xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info);
  if (!session_wrapper)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation), G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED, "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request =
      dex_await_object (xdp_request_dex_new (speech->context, app_info,
                                             G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
                        &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_get_provider_details (object, g_steal_pointer (&invocation),
                                                 xdp_request_dex_get_object_path (request));

  if (dex_await_boolean (ask_speech_permissions_future (speech, request, arg_parent_window), NULL))
    {
      SpeechProviderProxy *provider_proxy =
          g_hash_table_lookup (session_wrapper->providers, arg_provider_id);
      if (provider_proxy != NULL)
        {
          const char *provider_name = speech_provider_proxy_get_name (provider_proxy);
          g_variant_builder_add (&request_data_builder, "{sv}", "name",
                                 g_variant_new_string (provider_name));
          response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
        }
      else
        {
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        }
    }

  xdp_request_dex_emit_response (request, response, g_variant_builder_end (&request_data_builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static GVariant *
get_voices (SessionWrapper *session_wrapper)
{
  GHashTableIter providers_iter;
  SpeechProviderProxy *provider_proxy = NULL;
  char *provider_id = NULL;
  g_auto (GVariantBuilder) voices_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aa{sv}"));

  g_hash_table_iter_init (&providers_iter, session_wrapper->providers);
  while (g_hash_table_iter_next (&providers_iter, (gpointer *) &provider_id,
                                 (gpointer *) &provider_proxy))
    {
      GVariant *provider_voices = speech_provider_proxy_get_voices (provider_proxy);
      GVariant *voice_name = NULL;
      GVariant *voice_id = NULL;
      GVariant *voice_format = NULL;
      GVariant *voice_features = NULL;
      GVariant *voice_languages = NULL;

      g_assert_cmpstr (g_variant_get_type_string (provider_voices), ==, "a(ssstas)");

      g_autoptr (GVariantIter) iter = NULL;
      iter = g_variant_iter_new (provider_voices);

      while (g_variant_iter_loop (iter, "(@s@s@s@t@as)", &voice_name, &voice_id, &voice_format,
                                  &voice_features, &voice_languages))
        {
          GVariantDict dict;
          g_variant_dict_init (&dict, NULL);

          g_variant_dict_insert_value (&dict, "name", voice_name);
          g_variant_dict_insert_value (&dict, "identifier", voice_id);
          g_variant_dict_insert_value (&dict, "output-format", voice_format);
          g_variant_dict_insert_value (&dict, "features", voice_features);
          g_variant_dict_insert_value (&dict, "languages", voice_languages);
          g_variant_dict_insert (&dict, "provider", "s", provider_id);

          g_variant_builder_add_value (&voices_builder, g_variant_dict_end (&dict));
        }
    }

  return g_variant_builder_end (&voices_builder);
}

static gboolean
handle_get_voices (XdpDbusSpeech         *object,
                   GDBusMethodInvocation *invocation,
                   const char            *arg_session_handle,
                   const char            *arg_parent_window,
                   GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  XdpRequestDex *request = NULL;
  SessionWrapper *session_wrapper = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr (GError) error = NULL;

  session_wrapper =
      xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info);
  if (!session_wrapper)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation), G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED, "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request =
      dex_await_object (xdp_request_dex_new (speech->context, app_info,
                                             G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
                        &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_get_voices (object, g_steal_pointer (&invocation),
                                       xdp_request_dex_get_object_path (request));

  {
    guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
    g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (dex_await_boolean (ask_speech_permissions_future (speech, request, arg_parent_window),
                           NULL))
      {
        g_variant_builder_add (&request_data_builder, "{sv}", "voices",
                               get_voices (session_wrapper));

        response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
      }

    xdp_request_dex_emit_response (request, response,
                                   g_variant_builder_end (&request_data_builder));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_synthesize (XdpDbusSpeech         *object,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList           *fd_list,
                   const char            *arg_session_handle,
                   const char            *arg_parent_window,
                   const char            *arg_provider_id,
                   GVariant              *arg_pipe_fd,
                   const char            *arg_text,
                   const char            *arg_voice_id,
                   gdouble                arg_pitch,
                   gdouble                arg_rate,
                   gboolean               arg_is_ssml,
                   const char            *arg_language,
                   GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  XdpRequestDex *request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  SessionWrapper *session_wrapper = NULL;
  SpeechProviderProxy *provider_proxy = NULL;
  g_autoptr (GError) error = NULL;

  session_wrapper =
      xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info);
  if (!session_wrapper)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation), G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED, "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request =
      dex_await_object (xdp_request_dex_new (speech->context, app_info,
                                             G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
                        &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  provider_proxy = g_hash_table_lookup (session_wrapper->providers, arg_provider_id);

  xdp_dbus_speech_complete_synthesize (object, g_steal_pointer (&invocation), fd_list,
                                       xdp_request_dex_get_object_path (request));

  {
    guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
    g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!dex_await_boolean (ask_speech_permissions_future (speech, request, arg_parent_window),
                            NULL))
      {
        response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      }
    else if (!provider_proxy)
      {
        g_warning ("No provider found");
        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }
    else if (!dex_await (speech_provider_proxy_call_synthesize_future (
                             provider_proxy, arg_pipe_fd, arg_text, arg_voice_id, arg_pitch,
                             arg_rate, arg_is_ssml, arg_language, G_DBUS_CALL_FLAGS_NONE, -1,
                             fd_list),
                         &error))
      {
        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        g_variant_builder_add (&request_data_builder, "{sv}", "error-message",
                               g_variant_new_string (error->message));
      }

    xdp_request_dex_emit_response (request, response,
                                   g_variant_builder_end (&request_data_builder));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_speech_iface_init (XdpDbusSpeechIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_get_provider_details = handle_get_provider_details;
  iface->handle_get_voices = handle_get_voices;
  iface->handle_synthesize = handle_synthesize;
}

static void
xdp_speech_init (XdpSpeech *speech)
{
  xdp_dbus_speech_set_version (XDP_DBUS_SPEECH (speech), 1);
}

static void
xdp_speech_dispose (GObject *object)
{
  XdpSpeech *speech = XDP_SPEECH (object);

  g_clear_object (&speech->access_impl);
  g_clear_object (&speech->sessions);

  G_OBJECT_CLASS (xdp_speech_parent_class)->dispose (object);
}

static void
xdp_speech_class_init (XdpSpeechClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_speech_dispose;
}

static XdpSpeech *
xdp_speech_new (XdpContext *context, XdpDbusImplAccess *access_impl)
{
  XdpSpeech *speech;
  speech = g_object_new (XDP_TYPE_SPEECH, NULL);
  speech->context = context;
  speech->access_impl = g_object_ref (access_impl);
  speech->sessions = xdp_session_dex_store_new_wrapped (SessionWrapper, session);

  return speech;
}

DexFuture *
init_speech (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr (XdpSpeech) speech = NULL;
  XdpDbusImplAccess *access_impl;

  access_impl = xdp_context_get_access_impl (context);
  if (access_impl == NULL)
    {
      g_warning ("The speech portal requires an access impl");
      return dex_future_new_false ();
    }

  speech = xdp_speech_new (context, access_impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&speech)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
