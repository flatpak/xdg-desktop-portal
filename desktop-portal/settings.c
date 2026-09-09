/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "settings.h"

#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <glib/gi18n.h>

#include <libdex.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-dex.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

struct _XdpSettings
{
  XdpDbusSettingsSkeleton parent_instance;

  GPtrArray *impls; /* XdpDbusImplSettings */
  GCancellable *cancellable; /* (owned) (not nullable) */
};

#define XDP_TYPE_SETTINGS (xdp_settings_get_type ())
G_DECLARE_FINAL_TYPE (XdpSettings,
                      xdp_settings,
                      XDP, SETTINGS,
                      XdpDbusSettingsSkeleton)

static void xdp_settings_iface_init (XdpDbusSettingsIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSettings,
                               xdp_settings,
                               XDP_DBUS_TYPE_SETTINGS_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SETTINGS,
                                                      xdp_settings_iface_init));

static void
merge_impl_settings (GHashTable *merged,
                     GVariant   *settings)
{
  GVariantIter iter;
  const char *namespace;
  GVariant *nsvalue;

  g_variant_iter_init (&iter, settings);
  while (g_variant_iter_next (&iter, "{&s@a{sv}}", &namespace, &nsvalue))
    {
      g_autoptr (GVariant) owned_nsvalue = NULL;
      g_autofree char *owned_namespace = NULL;
      g_autoptr (GVariantDict) dict = NULL;
      GVariantIter iter2;
      const char *key;
      GVariant *value;

      owned_nsvalue = nsvalue;

      if (!g_hash_table_steal_extended (merged, namespace,
                                        (gpointer *)&owned_namespace,
                                        (gpointer *)&dict))
        {
          dict = g_variant_dict_new (NULL);
          owned_namespace = g_strdup (namespace);
        }

      g_variant_iter_init (&iter2, nsvalue);
      while (g_variant_iter_loop (&iter2, "{sv}", &key, &value))
        g_variant_dict_insert_value (dict, key, value);

      g_hash_table_insert (merged,
                           g_steal_pointer (&owned_namespace),
                           g_steal_pointer (&dict));
    }
}

static GVariant *
merged_to_variant (GHashTable *merged)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sa{sv}}"));
  const char *namespace;
  GVariantDict *dict;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, merged);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *)&namespace,
                                 (gpointer *)&dict))
    {
      g_variant_builder_add (&builder, "{s@a{sv}}",
                             namespace,
                             g_variant_dict_end (dict));
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* Every setting in @namespaces, higher priority implementations winning */
static GVariant *
xdp_settings_read_all (XdpSettings       *self,
                       const char *const *namespaces)
{
  g_autoptr(GHashTable) merged = NULL;

  merged = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  g_free,
                                  (GDestroyNotify) g_variant_dict_unref);

  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadAllResult) result = NULL;
      g_autoptr(GError) error = NULL;
      size_t j = self->impls->len - i - 1;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_all_future (g_ptr_array_index (self->impls, j),
                                                     namespaces),
        &error);

      if (result == NULL)
        g_warning ("Failed to ReadAll() from Settings implementation: %s",
                   error->message);
      else
        merge_impl_settings (merged, result->value);
    }

  return merged_to_variant (merged);
}

/* The value of @key from the highest priority implementation that has it */
static GVariant *
xdp_settings_read (XdpSettings *self,
                   const char  *namespace,
                   const char  *key)
{
  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadResult) result = NULL;
      g_autoptr(GError) error = NULL;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_future (g_ptr_array_index (self->impls, i),
                                                 namespace, key),
        &error);

      if (result != NULL)
        return g_variant_ref (result->value);

      g_debug ("Failed to Read() from Settings implementation: %s",
               error->message);
    }

  return NULL;
}

static gboolean
settings_handle_read_all (XdpDbusSettings       *object,
                          GDBusMethodInvocation *invocation,
                          const char    * const *arg_namespaces)
{
  XdpSettings *self = XDP_SETTINGS (object);
  g_autoptr(GVariant) settings = NULL;

  settings = xdp_settings_read_all (self, arg_namespaces);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new_tuple (&settings, 1));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
settings_handle_read (XdpDbusSettings       *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_namespace,
                      const char            *arg_key)
{
  XdpSettings *self = XDP_SETTINGS (object);
  g_autoptr(GVariant) value = NULL;

  g_debug ("Read %s %s", arg_namespace, arg_key);

  value = xdp_settings_read (self, arg_namespace, arg_key);
  if (value == NULL)
    {
      g_debug ("Attempted to read unknown namespace/key pair: %s %s",
               arg_namespace, arg_key);
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                     _("Requested setting not found"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(v)", value));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
settings_handle_read_one (XdpDbusSettings       *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_namespace,
                          const char            *arg_key)
{
  XdpSettings *self = XDP_SETTINGS (object);
  g_autoptr(GVariant) value = NULL;

  g_debug ("ReadOne %s %s", arg_namespace, arg_key);

  value = xdp_settings_read (self, arg_namespace, arg_key);
  if (value == NULL)
    {
      g_debug ("Attempted to read unknown namespace/key pair: %s %s",
               arg_namespace, arg_key);
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     XDG_DESKTOP_PORTAL_ERROR,
                                                     XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                     _("Requested setting not found"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new_tuple (&value, 1));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static DexFuture *
settings_changed_fiber (gpointer      self_ptr,
                        unsigned int  impl_idx,
                        const char   *ns,
                        const char   *key,
                        GVariant     *value)
{
  /* self is not owned here, instead we cancel on dispose */
  XdpSettings *self = XDP_SETTINGS (self_ptr);

  /* Check if any higher priority impl provides this key; suppress if so */
  for (size_t i = 0; i < impl_idx; i++)
    {
      g_autoptr(XdpDbusImplSettingsReadResult) result = NULL;
      g_autoptr(GError) error = NULL;

      result = dex_await_boxed (
        xdp_dbus_impl_settings_call_read_future (g_ptr_array_index (self->impls, i),
                                                 ns, key),
        &error);

      if (result != NULL)
        return dex_future_new_for_boolean (FALSE);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return dex_future_new_for_boolean (FALSE);
    }

  g_debug ("Emitting changed for %s %s", ns, key);
  xdp_dbus_settings_emit_setting_changed (XDP_DBUS_SETTINGS (self),
                                          ns, key, value);

  return dex_future_new_for_boolean (TRUE);
}

static void
on_impl_settings_changed (XdpDbusImplSettings *impl,
                          const char          *arg_namespace,
                          const char          *arg_key,
                          GVariant            *arg_value,
                          XdpSettings         *self)
{
  unsigned int impl_idx;

  g_ptr_array_find (self->impls, impl, &impl_idx);

  if (impl_idx == 0)
    {
      g_debug ("Emitting changed for %s %s", arg_namespace, arg_key);
      xdp_dbus_settings_emit_setting_changed (XDP_DBUS_SETTINGS (self),
                                              arg_namespace, arg_key,
                                              arg_value);
      return;
    }

  dex_future_disown (
    dex_future_first (
      dex_scheduler_spawnv (NULL, 0,
                            G_CALLBACK (settings_changed_fiber),
                            5,
                            G_TYPE_POINTER, self,
                            G_TYPE_UINT, impl_idx,
                            G_TYPE_STRING, arg_namespace,
                            G_TYPE_STRING, arg_key,
                            G_TYPE_VARIANT, arg_value),
      dex_cancellable_new_from_cancellable (self->cancellable),
      NULL));
}

static void
xdp_settings_iface_init (XdpDbusSettingsIface *iface)
{
  iface->handle_read = settings_handle_read;
  iface->handle_read_one = settings_handle_read_one;
  iface->handle_read_all = settings_handle_read_all;
}

static void
xdp_settings_init (XdpSettings *self)
{
}

static void
xdp_settings_dispose (GObject *object)
{
  XdpSettings *self = XDP_SETTINGS (object);

  for (size_t i = 0; self->impls && i < self->impls->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->impls, i), self);

  g_clear_pointer (&self->impls, g_ptr_array_unref);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (xdp_settings_parent_class)->dispose (object);
}

static void
xdp_settings_class_init (XdpSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_settings_dispose;
}

static XdpSettings *
xdp_settings_new (GPtrArray *impls)
{
  XdpSettings *self;

  self = g_object_new (XDP_TYPE_SETTINGS, NULL);
  self->cancellable = g_cancellable_new ();
  self->impls = g_ptr_array_ref (impls);

  xdp_dbus_settings_set_version (XDP_DBUS_SETTINGS (self), 2);

  for (size_t i = 0; i < self->impls->len; i++)
    {
      g_signal_connect_object (g_ptr_array_index (self->impls, i),
                               "setting-changed",
                               G_CALLBACK (on_impl_settings_changed),
                               self,
                               G_CONNECT_DEFAULT);
    }

  return self;
}

static GPtrArray *
create_impl_proxies (GDBusConnection *connection,
                     GPtrArray       *impl_configs)
{
  g_autoptr(GPtrArray) futures = g_ptr_array_new_with_free_func (dex_unref);
  g_autoptr(GPtrArray) impl_proxies =
    g_ptr_array_new_with_free_func (g_object_unref);

  for (size_t i = 0; i < impl_configs->len; i++)
    {
      XdpImplConfig *impl_config;
      g_autoptr(DexFuture) future = NULL;

      impl_config = g_ptr_array_index (impl_configs, i);
      future = xdp_dbus_impl_settings_proxy_new_future (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        impl_config->dbus_name,
                                                        DESKTOP_DBUS_PATH);
      g_ptr_array_add (futures, g_steal_pointer (&future));
    }

  dex_await (dex_future_allv ((DexFuture *const *) futures->pdata, futures->len), NULL);

  for (size_t i = 0; i < futures->len; i++)
    {
      DexFuture *future = g_ptr_array_index (futures, i);
      g_autoptr(GError) error = NULL;
      const GValue *value;

      value = dex_future_get_value (future, &error);
      if (value == NULL)
        g_warning ("Failed to create settings proxy: %s", error->message);
      else
        g_ptr_array_add (impl_proxies, g_object_ref (g_value_get_object (value)));
    }

  return g_steal_pointer (&impl_proxies);
}

DexFuture *
init_settings (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(XdpSettings) settings = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  g_autoptr(GPtrArray) impl_configs = NULL;
  g_autoptr(GPtrArray) impl_proxies = NULL;

  impl_configs = xdp_portal_config_find_all (config, SETTINGS_DBUS_IMPL_IFACE);
  if (impl_configs->len == 0)
    return dex_future_new_true ();

  impl_proxies = create_impl_proxies (connection, impl_configs);

  if (impl_proxies->len == 0)
    {
      g_warning ("Not providing Settings portal: No working backend");
      return dex_future_new_false ();
    }

  settings = xdp_settings_new (impl_proxies);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&settings)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}

#if HAVE_VARLINK

#define VARLINK_INTERFACE "org.freedesktop.portal.Settings"

static const char varlink_interface_description[] =
  "# This interface provides read-only access to a small number of\n"
  "# standardized host settings required for toolkits similar to XSettings. It\n"
  "# is not for general purpose settings.\n"
  "#\n"
  "# Implementations can provide keys beyond the standardized ones; they are\n"
  "# entirely implementation details that are undocumented.\n"
  "#\n"
  "# Values are objects with a single `v` field, mirroring the D-Bus variant\n"
  "# they replace: {\"v\": true}\n"
  "interface " VARLINK_INTERFACE "\n"
  "\n"
  "# If namespaces is an empty array or contains an empty string it matches\n"
  "# all. Globbing is supported but only for trailing sections, e.g.\n"
  "# \"org.example.*\"\n"
  "method ReadAll(namespaces: []string) -> (values: []Namespace)\n"
  "\n"
  "# Reads a single value which may be any valid type. Returns an error on any\n"
  "# unknown namespace or key\n"
  "method Read(namespace: string, key: string) -> (value: object)\n"
  "\n"
  "# Reports a setting changing. Call with `more` on a dedicated connection;\n"
  "# the first replies enumerate the matching settings, and later ones report\n"
  "# changes in the same shape\n"
  "method SubscribeSettingChanged(namespaces: []string) -> (\n"
  "  namespace: string,\n"
  "  key: string,\n"
  "  value: object\n"
  ")\n"
  "\n"
  "# A namespace with its keys and values\n"
  "type Namespace (\n"
  "  namespace: string,\n"
  "  values: [string]object\n"
  ")\n"
  "\n"
  "# An unknown namespace or key\n"
  "error KeyNotFound()\n"
  "error ExpectedMore()\n"
  "error " XDP_VARLINK_ERROR_NOT_REGISTERED "()\n";

typedef struct _VarlinkSettings VarlinkSettings;

/* One per SubscribeSettingChanged call, living until its connection closes */
typedef struct
{
  VarlinkSettings *varlink_settings;

  /* Owned, and NULL once the connection is gone */
  VarlinkCall *call;

  GStrv namespaces;

  /* Held back during the initial enumeration, so a change arriving then is
   * reported after the value it replaces */
  GPtrArray *pending;

  /* The subscribing handler owns it while it is still enumerating */
  gboolean subscribing;
} Subscription;

struct _VarlinkSettings
{
  XdpSettings *settings;
  gulong changed_id;

  /* Not owned; each subscription removes itself when its connection closes */
  GPtrArray *subscriptions;
};

static void
varlink_object_free (gpointer data)
{
  varlink_object_unref (data);
}

static void
subscription_free (Subscription *subscription)
{
  g_clear_pointer (&subscription->call, varlink_call_unref);
  g_clear_pointer (&subscription->pending, g_ptr_array_unref);
  g_clear_pointer (&subscription->namespaces, g_strfreev);
  g_free (subscription);
}

static void
subscription_close (Subscription *subscription)
{
  g_ptr_array_remove_fast (subscription->varlink_settings->subscriptions,
                           subscription);
  g_clear_pointer (&subscription->call, varlink_call_unref);

  if (!subscription->subscribing)
    subscription_free (subscription);
}

static void
on_call_connection_closed (VarlinkCall *call,
                           void        *user_data)
{
  subscription_close (user_data);
}

/* An empty list matches everything, as does a trailing `*` on a prefix */
static gboolean
namespace_matches (const char *const *namespaces,
                   const char        *namespace)
{
  if (namespaces == NULL || namespaces[0] == NULL)
    return TRUE;

  for (size_t i = 0; namespaces[i] != NULL; i++)
    {
      const char *pattern = namespaces[i];
      size_t length = strlen (pattern);

      if (length == 0)
        return TRUE;

      if (pattern[length - 1] == '*')
        {
          if (strncmp (pattern, namespace, length - 1) == 0)
            return TRUE;
        }
      else if (strcmp (pattern, namespace) == 0)
        {
          return TRUE;
        }
    }

  return FALSE;
}

static GStrv
get_namespaces (VarlinkObject *parameters)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  VarlinkArray *array;
  unsigned long n_elements;

  if (varlink_object_get_array (parameters, "namespaces", &array) < 0)
    return NULL;

  n_elements = varlink_array_get_n_elements (array);
  for (unsigned long i = 0; i < n_elements; i++)
    {
      const char *namespace;

      if (varlink_array_get_string (array, i, &namespace) < 0)
        return NULL;

      g_strv_builder_add (builder, namespace);
    }

  return g_strv_builder_end (builder);
}

static VarlinkObject *
changed_reply (const char *namespace,
               const char *key,
               GVariant   *value)
{
  g_autoptr(VarlinkObject) object = NULL;
  g_autoptr(VarlinkObject) reply = NULL;

  object = xdp_varlink_object_new_for_variant (value);
  if (object == NULL)
    return NULL;

  varlink_object_new (&reply);
  varlink_object_set_string (reply, "namespace", namespace);
  varlink_object_set_string (reply, "key", key);
  varlink_object_set_object (reply, "value", object);

  return g_steal_pointer (&reply);
}

static void
on_setting_changed (XdpSettings     *settings,
                    const char      *namespace,
                    const char      *key,
                    GVariant        *value,
                    VarlinkSettings *self)
{
  g_autoptr(GVariant) inner = g_variant_get_variant (value);

  for (size_t i = 0; i < self->subscriptions->len; i++)
    {
      Subscription *subscription = g_ptr_array_index (self->subscriptions, i);
      g_autoptr(VarlinkObject) reply = NULL;

      if (!namespace_matches ((const char *const *) subscription->namespaces,
                              namespace))
        continue;

      reply = changed_reply (namespace, key, inner);
      if (reply == NULL)
        continue;

      if (subscription->pending != NULL)
        g_ptr_array_add (subscription->pending, g_steal_pointer (&reply));
      else
        xdp_varlink_call_reply (subscription->call, reply,
                                VARLINK_REPLY_CONTINUES);
    }
}

static long
handle_varlink_read_all (XdpVarlinkService    *service,
                         XdpVarlinkConnection *connection,
                         VarlinkCall          *call,
                         VarlinkObject        *parameters,
                         uint64_t              flags,
                         gpointer              user_data)
{
  VarlinkSettings *self = user_data;
  g_auto(GStrv) namespaces = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  g_autoptr(VarlinkArray) values = NULL;
  g_autoptr(GVariant) settings = NULL;
  GVariantIter iter;
  const char *namespace;
  GVariant *nsvalue;

  namespaces = get_namespaces (parameters);
  if (namespaces == NULL)
    return varlink_call_reply_invalid_parameter (call, "namespaces");

  settings = xdp_settings_read_all (self->settings,
                                    (const char *const *) namespaces);

  varlink_array_new (&values);
  g_variant_iter_init (&iter, settings);
  while (g_variant_iter_loop (&iter, "{&s@a{sv}}", &namespace, &nsvalue))
    {
      g_autoptr(VarlinkObject) entry = NULL;
      g_autoptr(VarlinkObject) ns_values = NULL;
      GVariantIter value_iter;
      const char *key;
      GVariant *value;

      varlink_object_new (&ns_values);

      g_variant_iter_init (&value_iter, nsvalue);
      while (g_variant_iter_loop (&value_iter, "{&sv}", &key, &value))
        {
          g_autoptr(VarlinkObject) object = NULL;

          object = xdp_varlink_object_new_for_variant (value);
          if (object != NULL)
            varlink_object_set_object (ns_values, key, object);
        }

      varlink_object_new (&entry);
      varlink_object_set_string (entry, "namespace", namespace);
      varlink_object_set_object (entry, "values", ns_values);
      varlink_array_append_object (values, entry);
    }

  varlink_object_new (&reply);
  varlink_object_set_array (reply, "values", values);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_read (XdpVarlinkService    *service,
                     XdpVarlinkConnection *connection,
                     VarlinkCall          *call,
                     VarlinkObject        *parameters,
                     uint64_t              flags,
                     gpointer              user_data)
{
  VarlinkSettings *self = user_data;
  g_autoptr(GVariant) value = NULL;
  g_autoptr(GVariant) inner = NULL;
  g_autoptr(VarlinkObject) object = NULL;
  g_autoptr(VarlinkObject) reply = NULL;
  const char *namespace;
  const char *key;

  if (varlink_object_get_string (parameters, "namespace", &namespace) < 0)
    return varlink_call_reply_invalid_parameter (call, "namespace");

  if (varlink_object_get_string (parameters, "key", &key) < 0)
    return varlink_call_reply_invalid_parameter (call, "key");

  value = xdp_settings_read (self->settings, namespace, key);
  if (value == NULL)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".KeyNotFound", NULL);

  inner = g_variant_get_variant (value);
  object = xdp_varlink_object_new_for_variant (inner);
  if (object == NULL)
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".KeyNotFound", NULL);

  varlink_object_new (&reply);
  varlink_object_set_object (reply, "value", object);

  return varlink_call_reply (call, reply, 0);
}

static long
handle_varlink_subscribe (XdpVarlinkService    *service,
                          XdpVarlinkConnection *connection,
                          VarlinkCall          *call,
                          VarlinkObject        *parameters,
                          uint64_t              flags,
                          gpointer              user_data)
{
  VarlinkSettings *self = user_data;
  Subscription *subscription;
  g_auto(GStrv) namespaces = NULL;
  g_autoptr(GVariant) settings = NULL;
  GVariantIter iter;
  const char *namespace;
  GVariant *nsvalue;

  if (!(flags & VARLINK_CALL_MORE))
    return varlink_call_reply_error (call, VARLINK_INTERFACE ".ExpectedMore", NULL);

  namespaces = get_namespaces (parameters);
  if (namespaces == NULL)
    return varlink_call_reply_invalid_parameter (call, "namespaces");

  /* Registered before the enumeration reads, so a change landing during it
   * is queued rather than lost */
  subscription = g_new0 (Subscription, 1);
  subscription->varlink_settings = self;
  subscription->call = varlink_call_ref (call);
  subscription->namespaces = g_strdupv (namespaces);
  subscription->pending = g_ptr_array_new_with_free_func (varlink_object_free);
  subscription->subscribing = TRUE;
  g_ptr_array_add (self->subscriptions, subscription);
  varlink_call_set_connection_closed_callback (call, on_call_connection_closed,
                                               subscription);

  settings = xdp_settings_read_all (self->settings,
                                    (const char *const *) namespaces);
  subscription->subscribing = FALSE;

  if (subscription->call == NULL)
    {
      subscription_free (subscription);
      return 0;
    }

  g_variant_iter_init (&iter, settings);
  while (g_variant_iter_loop (&iter, "{&s@a{sv}}", &namespace, &nsvalue))
    {
      GVariantIter value_iter;
      const char *key;
      GVariant *value;

      g_variant_iter_init (&value_iter, nsvalue);
      while (g_variant_iter_loop (&value_iter, "{&sv}", &key, &value))
        {
          g_autoptr(VarlinkObject) reply = changed_reply (namespace, key, value);

          if (reply != NULL)
            varlink_call_reply (call, reply, VARLINK_REPLY_CONTINUES);
        }
    }

  for (size_t i = 0; i < subscription->pending->len; i++)
    {
      varlink_call_reply (call, g_ptr_array_index (subscription->pending, i),
                          VARLINK_REPLY_CONTINUES);
    }

  g_clear_pointer (&subscription->pending, g_ptr_array_unref);

  return 0;
}

static const XdpVarlinkMethod varlink_methods[] = {
  { "ReadAll", handle_varlink_read_all, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "Read", handle_varlink_read, XDP_VARLINK_METHOD_FLAGS_NONE },
  { "SubscribeSettingChanged", handle_varlink_subscribe, XDP_VARLINK_METHOD_FLAGS_NONE },
};

static void
varlink_settings_free (gpointer data)
{
  VarlinkSettings *self = data;

  g_clear_signal_handler (&self->changed_id, self->settings);
  g_clear_pointer (&self->subscriptions, g_ptr_array_unref);
  g_clear_object (&self->settings);
  g_free (self);
}

gboolean
init_settings_varlink (XdpVarlinkService  *service,
                       XdpContext         *context,
                       GError            **error)
{
  GDBusInterfaceSkeleton *skeleton;
  VarlinkSettings *self;

  skeleton = xdp_context_get_portal (context, SETTINGS_DBUS_IFACE);
  if (skeleton == NULL)
    return TRUE;

  self = g_new0 (VarlinkSettings, 1);
  self->settings = g_object_ref (XDP_SETTINGS (skeleton));
  self->subscriptions = g_ptr_array_new ();
  self->changed_id = g_signal_connect (self->settings,
                                       "setting-changed",
                                       G_CALLBACK (on_setting_changed),
                                       self);

  return xdp_varlink_service_add_interface (service,
                                            varlink_interface_description,
                                            varlink_methods,
                                            G_N_ELEMENTS (varlink_methods),
                                            self,
                                            varlink_settings_free,
                                            error);
}

#endif /* HAVE_VARLINK */
