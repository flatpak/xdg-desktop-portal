/*
 * Copyright © 2022 Matthew Leeds
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
 *       Matthew Leeds <mwleeds@protonmail.com>
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
#include <gio/gunixinputstream.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "dynamic-launcher.h"
#include "request.h"
#include "call.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define MAX_DESKTOP_SIZE_BYTES 1048576

typedef struct _DynamicLauncher DynamicLauncher;
typedef struct _DynamicLauncherClass DynamicLauncherClass;

struct _DynamicLauncher
{
  XdpDbusDynamicLauncherSkeleton parent_instance;
};

struct _DynamicLauncherClass
{
  XdpDbusDynamicLauncherSkeletonClass parent_class;
};

static XdpDbusImplDynamicLauncher *impl;
static DynamicLauncher *dynamic_launcher;

static GMutex transient_permissions_lock;
static GHashTable *transient_permissions;

GType dynamic_launcher_get_type (void) G_GNUC_CONST;
static void dynamic_launcher_iface_init (XdpDbusDynamicLauncherIface *iface);

G_DEFINE_TYPE_WITH_CODE (DynamicLauncher, dynamic_launcher,
                         XDP_DBUS_TYPE_DYNAMIC_LAUNCHER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_DYNAMIC_LAUNCHER,
                                                dynamic_launcher_iface_init));

typedef enum {
  DYNAMIC_LAUNCHER_TYPE_APPLICATION = 1,
  DYNAMIC_LAUNCHER_TYPE_WEBAPP = 2,
} DynamicLauncherType;

static GVariant *
get_launcher_data_and_revoke_token (const char *token)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);
  GVariant *launcher_data_wrapped;

  if (!transient_permissions)
    return NULL;

  if (!g_uuid_string_is_valid (token))
    return NULL;

  launcher_data_wrapped = g_hash_table_lookup (transient_permissions, token);
  if (launcher_data_wrapped)
    {
      g_autoptr(GVariant) launcher_data = NULL;
      guint timeout_id;

      g_variant_get (launcher_data_wrapped, "(vu)", &launcher_data, &timeout_id);

      g_source_remove (timeout_id);
      g_hash_table_remove (transient_permissions, token);

      return g_steal_pointer (&launcher_data);
    }

  return NULL;
}

static gboolean
validate_desktop_file_id (const char  *app_id,
                          const char  *desktop_file_id,
                          GError     **error)
{
  const char *after_app_id;

  if (!g_str_has_suffix (desktop_file_id, ".desktop"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Desktop file id missing .desktop suffix: %s"), desktop_file_id);
      return FALSE;
    }

  if (app_id == NULL || *app_id == '\0')
    return TRUE;

  after_app_id = desktop_file_id + strlen (app_id);
  if (!g_str_has_prefix (desktop_file_id, app_id) || *after_app_id != '.')
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Desktop file id missing app id prefix '%s.': %s"),
                   app_id, desktop_file_id);
      return FALSE;
    }

  return TRUE;
}

static gboolean
write_icon_to_disk (GVariant    *icon_v,
                    const char  *icon_subdir,
                    const char  *icon_path,
                    GError     **error)
{
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GFile) icon_file = NULL;
  g_autoptr(GFileOutputStream) icon_stream = NULL;
  GBytes *icon_bytes;
  gconstpointer bytes_data;
  gsize bytes_len;

  icon = g_icon_deserialize (icon_v);
  g_assert (G_IS_BYTES_ICON (icon));
  icon_bytes = g_bytes_icon_get_bytes (G_BYTES_ICON (icon));

  g_mkdir_with_parents (icon_subdir, 0700);
  icon_file = g_file_new_for_path (icon_path);
  icon_stream = g_file_replace (icon_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, error);
  if (icon_stream == NULL)
    return FALSE;

  /* Use write_all() instead of write_bytes() so we don't have to worry about
   * partial writes (https://gitlab.gnome.org/GNOME/glib/-/issues/570).
   */
  bytes_data = g_bytes_get_data (icon_bytes, &bytes_len);
  if (!g_output_stream_write_all (G_OUTPUT_STREAM (icon_stream),
                                  bytes_data, bytes_len,
                                  NULL, NULL, error) ||
      !g_output_stream_close (G_OUTPUT_STREAM (icon_stream), NULL, error))
    return FALSE;

  return TRUE;
}

static GKeyFile *
save_icon_and_get_desktop_entry (const char  *desktop_file_id,
                                 const char  *desktop_entry,
                                 GVariant    *launcher_data,
                                 XdpAppInfo  *xdp_app_info,
                                 char       **out_icon_path,
                                 GError     **error)
{
  g_autoptr(GVariant) icon_v = NULL;
  g_autoptr(GDesktopAppInfo) desktop_app_info = NULL;
  g_autoptr(GKeyFile) key_file = g_key_file_new ();
  g_autofree char *exec = NULL;
  g_auto(GStrv) exec_strv = NULL;
  g_auto(GStrv) prefixed_exec_strv = NULL;
  g_auto(GStrv) groups = NULL;
  g_autofree char *prefixed_exec = NULL;
  g_autofree char *tryexec_path = NULL;
  g_autofree char *icon_path = NULL;
  g_autofree char *icon_subdir = NULL;
  const char *name, *icon_extension, *icon_size;
  const char *app_id;

  g_variant_get (launcher_data, "(&sv&s&s)", &name, &icon_v, &icon_extension, &icon_size);
  g_assert (name != NULL && name[0] != '\0');
  g_assert (icon_v);
  g_assert (icon_extension != NULL && icon_extension[0] != '\0');
  g_assert (icon_size != NULL && icon_size[0] != '\0');

  app_id = xdp_app_info_get_id (xdp_app_info);

  if (!g_key_file_load_from_data (key_file, desktop_entry, -1,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  error))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Desktop entry given to Install() not a valid key file"));
      return NULL;
    }

  /* The desktop entry spec supports more than one group but we don't in case
   * there's a security risk.
   */
  groups = g_key_file_get_groups (key_file, NULL);
  if (g_strv_length (groups) > 1 || !g_strv_contains ((const char * const *)groups, G_KEY_FILE_DESKTOP_GROUP))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Desktop entry given to Install() must have only one group"));
      return NULL;
    }

  /* Overwrite Name= and Icon= if they are present */
  g_key_file_set_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "Name", name);

  {
    g_autofree char *no_dot_desktop = NULL;
    g_autofree char *icon_name = NULL;
    g_autofree char *subdir = NULL;

    no_dot_desktop = g_strndup (desktop_file_id, strlen(desktop_file_id) - strlen (".desktop"));
    icon_name = g_strconcat (no_dot_desktop, ".", icon_extension, NULL);

    /* Put the icon in a per-size subdirectory so the size is discernible
     * without reading the file
     */
    if (g_strcmp0 (icon_extension, "svg") == 0)
      subdir = g_strdup ("scalable");
    else
      subdir = g_strdup_printf ("%sx%s", icon_size, icon_size);

    icon_subdir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_ICONS_DIR, subdir, NULL);
    icon_path = g_build_filename (icon_subdir, icon_name, NULL);

    g_key_file_set_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "Icon", icon_path);
  }

  exec = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "Exec", error);
  if (exec == NULL)
    return NULL;

  if (!g_shell_parse_argv (exec, NULL, &exec_strv, error))
    return NULL;

  /* Don't let the app give itself access to host files */
  if (xdp_app_info_get_kind (xdp_app_info) == XDP_APP_INFO_KIND_FLATPAK &&
      g_strv_contains ((const char * const *)exec_strv, "--file-forwarding"))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Desktop entry given to Install() must not use --file-forwarding"));
      return NULL;
    }

  prefixed_exec_strv = xdp_app_info_rewrite_commandline (xdp_app_info,
                                                         (const char * const *)exec_strv,
                                                         TRUE /* quote escape */);
  if (prefixed_exec_strv == NULL)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("DynamicLauncher install not supported for: %s"), app_id);
      return NULL;
    }

  prefixed_exec = g_strjoinv (" ", prefixed_exec_strv);
  g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "Exec", prefixed_exec);

  tryexec_path = xdp_app_info_get_tryexec_path (xdp_app_info);
  if (tryexec_path != NULL)
    g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "TryExec", tryexec_path);

  if (xdp_app_info_get_kind (xdp_app_info) == XDP_APP_INFO_KIND_FLATPAK)
    {
      /* Flatpak checks for this key */
      g_key_file_set_value (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", app_id);
      /* Flatpak removes this one for security */
      g_key_file_remove_key (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-GNOME-Bugzilla-ExtraInfoScript", NULL);
    }

  desktop_app_info = g_desktop_app_info_new_from_keyfile (key_file);
  if (desktop_app_info == NULL)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Desktop entry given to Install() not valid"));
      return NULL;
    }

  /* Write the icon last so it's only on-disk if other checks passed */
  if (!write_icon_to_disk (icon_v, icon_subdir, icon_path, error))
    return NULL;

  if (out_icon_path)
    *out_icon_path = g_steal_pointer (&icon_path);

  return g_steal_pointer (&key_file);
}

static gboolean
handle_install (XdpDbusDynamicLauncher *object,
                GDBusMethodInvocation  *invocation,
                const gchar            *arg_token,
                const gchar            *arg_desktop_file_id,
                const gchar            *arg_desktop_entry,
                GVariant               *arg_options)
{
  Call *call = call_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (call->app_info);
  g_autoptr(GVariant) launcher_data = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) desktop_keyfile = NULL;
  g_autofree char *icon_path = NULL;
  g_autofree char *desktop_dir = NULL;
  g_autofree char *desktop_path = NULL;
  g_autofree char *link_path = NULL;
  g_autofree char *relative_path = NULL;
  g_autoptr(GFile) link_file = NULL;
  gsize desktop_entry_length = G_MAXSIZE;

  launcher_data = get_launcher_data_and_revoke_token (arg_token);
  if (launcher_data == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             _("Token given is invalid: %s"), arg_token);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }


  if (!validate_desktop_file_id (app_id, arg_desktop_file_id, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  desktop_keyfile = save_icon_and_get_desktop_entry (arg_desktop_file_id,
                                                     arg_desktop_entry,
                                                     launcher_data,
                                                     call->app_info,
                                                     &icon_path,
                                                     &error);
  if (desktop_keyfile == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_free (g_key_file_to_data (desktop_keyfile, &desktop_entry_length, NULL));
  if (desktop_entry_length > MAX_DESKTOP_SIZE_BYTES)
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("Desktop file exceeds max size (%d): %s"),
                   MAX_DESKTOP_SIZE_BYTES, arg_desktop_file_id);
      goto error;
    }

  /* Put the desktop file in ~/.local/share/xdg-desktop-portal/applications/ so
   * there's no ambiguity about which launchers were created by this portal.
   */
  desktop_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_APPLICATIONS_DIR, NULL);
  g_mkdir_with_parents (desktop_dir, 0700);
  desktop_path = g_build_filename (desktop_dir, arg_desktop_file_id, NULL);
  if (!g_key_file_save_to_file (desktop_keyfile, desktop_path, &error))
    goto error;

  /* Make a sym link in ~/.local/share/applications so the launcher shows up in
   * the desktop environment's menu.
   */
  link_path = g_build_filename (g_get_user_data_dir (), "applications", arg_desktop_file_id, NULL);
  link_file = g_file_new_for_path (link_path);
  relative_path = g_build_filename ("..", XDG_PORTAL_APPLICATIONS_DIR, arg_desktop_file_id, NULL);
  g_file_delete (link_file, NULL, NULL);
  if (!g_file_make_symbolic_link (link_file, relative_path, NULL, &error))
    goto error;

  xdp_dbus_dynamic_launcher_complete_install (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;

error:
  g_dbus_method_invocation_return_gerror (invocation, error);
  remove (icon_path);
  remove (desktop_path);
  remove (link_path);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey response_options[] = {
  { "name", G_VARIANT_TYPE_STRING, NULL },
  { "icon", G_VARIANT_TYPE_VARIANT, NULL },
  { "token", G_VARIANT_TYPE_UINT32, NULL }
};

static gboolean
install_token_timeout (gpointer data)
{
  g_autoptr(GVariant) launcher_data = NULL;

  g_debug ("Revoking install token %s", (char *)data);
  launcher_data = get_launcher_data_and_revoke_token ((char *)data);
  g_free (data);

  return G_SOURCE_REMOVE;
}

static void
set_launcher_data_for_token (const char *token,
                             GVariant   *launcher_data)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&transient_permissions_lock);
  guint timeout_id;
  GVariant *launcher_data_wrapped;

  if (!transient_permissions)
    {
      transient_permissions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, (GDestroyNotify)g_variant_unref);
    }

  /* Revoke the token if it hasn't been used after 5 minutes, in case of
   * client bugs. This is what the GNOME print portal implementation does.
   */
  timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 300, install_token_timeout,
                                           g_strdup (token), g_free);
  launcher_data_wrapped = g_variant_new ("(vu)", launcher_data, timeout_id);

  g_hash_table_insert (transient_permissions,
                       g_strdup (token),
                       g_variant_ref_sink (launcher_data_wrapped));
}

static void
prepare_install_done (GObject      *source,
                      GAsyncResult *result,
                      gpointer      data)
{
  g_autoptr(Request) request = data;
  GVariant *launcher_data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder results_builder;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (!xdp_dbus_impl_dynamic_launcher_call_prepare_install_finish (XDP_DBUS_IMPL_DYNAMIC_LAUNCHER (source),
                                                                   &response,
                                                                   &results,
                                                                   result,
                                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
      goto out;
    }

  if (request->exported && response == 0)
    {
      g_autofree char *token = g_uuid_string_random ();
      const char *chosen_name = NULL;
      const char *icon_format = NULL;
      const char *icon_size = NULL;
      GVariant *chosen_icon = NULL;

      icon_format = g_object_get_data (G_OBJECT (request), "icon-format");
      g_assert (icon_format != NULL && icon_format[0] != '\0');
      icon_size = g_object_get_data (G_OBJECT (request), "icon-size");
      g_assert (icon_size != NULL && icon_size[0] != '\0');

      if (!xdp_filter_options (results, &results_builder,
                               response_options, G_N_ELEMENTS (response_options),
                               &error) ||
          !g_variant_lookup (results, "name", "&s", &chosen_name) ||
          chosen_name[0] == '\0' ||
          !g_variant_lookup (results, "icon", "v", &chosen_icon))
        {
          g_warning ("Results from backend failed validation: %s",
                     error ? error->message : "missing entries");
          response = 2;
        }
      else
        {
          /* Save the token in memory and return it to the caller */
          launcher_data = g_variant_new ("(svss)", chosen_name, chosen_icon, icon_format, icon_size);
          set_launcher_data_for_token (token, launcher_data);
          g_variant_builder_add (&results_builder, "{sv}", "token", g_variant_new_string (token));
        }
    }

out:
  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&results_builder));

      request_unexport (request);
    }
  else
    {
      g_variant_builder_clear (&results_builder);
    }
}

static gboolean
validate_url (const char  *key,
              GVariant    *value,
              GVariant    *options,
              GError     **error)
{
  const char *url = g_variant_get_string (value, NULL);
  g_autoptr(GError) local_error = NULL;
  guint32 launcher_type;

  g_variant_lookup (options, "launcher_type", "u", &launcher_type);
  if (launcher_type == DYNAMIC_LAUNCHER_TYPE_WEBAPP &&
      !g_uri_is_valid (url, G_URI_FLAGS_NONE, &local_error))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("URL given is invalid: %s"), local_error->message);
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_launcher_type (const char  *key,
                        GVariant    *value,
                        GVariant    *options,
                        GError     **error)
{
  guint32 launcher_type = g_variant_get_uint32 (value);
  guint32 supported_launcher_types;

  supported_launcher_types =
    xdp_dbus_dynamic_launcher_get_supported_launcher_types
    (XDP_DBUS_DYNAMIC_LAUNCHER (dynamic_launcher));

  if (__builtin_popcount (launcher_type) != 1)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Invalid launcher type: %x"), launcher_type);
      return FALSE;
    }

  if (!(supported_launcher_types & launcher_type))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("Unsupported launcher type: %x"), launcher_type);
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey prepare_install_options[] = {
  { "modal", G_VARIANT_TYPE_BOOLEAN },
  { "launcher_type", G_VARIANT_TYPE_UINT32, validate_launcher_type },
  { "target", G_VARIANT_TYPE_STRING, validate_url },
  { "editable_name", G_VARIANT_TYPE_BOOLEAN },
  { "editable_icon", G_VARIANT_TYPE_BOOLEAN }
};

static gboolean
handle_prepare_install (XdpDbusDynamicLauncher *object,
                        GDBusMethodInvocation  *invocation,
                        const gchar            *arg_parent_window,
                        const gchar            *arg_name,
                        GVariant               *arg_icon_v,
                        GVariant               *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariantBuilder opt_builder;
  g_autofree char *icon_format = NULL;
  g_autofree char *icon_size = NULL;
  g_autoptr(GVariant) icon_v = NULL;

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

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &opt_builder,
                           prepare_install_options, G_N_ELEMENTS (prepare_install_options), &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* Do some validation on the icon before passing it along */
  icon_v = g_variant_get_variant (arg_icon_v);
  if (!icon_v || !xdp_validate_serialized_icon (icon_v, TRUE /* bytes_only */,
                                                &icon_format, &icon_size))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             _("Dynamic launcher icon failed validation"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data_full (G_OBJECT (request), "icon-format", g_steal_pointer (&icon_format), g_free);
  g_object_set_data_full (G_OBJECT (request), "icon-size", g_steal_pointer (&icon_size), g_free);

  xdp_dbus_impl_dynamic_launcher_call_prepare_install (impl,
                                                       request->id,
                                                       app_id,
                                                       arg_parent_window,
                                                       arg_name,
                                                       arg_icon_v,
                                                       g_variant_builder_end (&opt_builder),
                                                       NULL, /* cancellable */
                                                       prepare_install_done,
                                                       g_object_ref (request));

  xdp_dbus_dynamic_launcher_complete_prepare_install (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_request_install_token (XdpDbusDynamicLauncher *object,
                              GDBusMethodInvocation  *invocation,
                              const gchar            *arg_name,
                              GVariant               *arg_icon_v,
                              GVariant               *arg_options)
{
  Call *call = call_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (call->app_info);
  g_autoptr(GError) error = NULL;
  GVariant *launcher_data;
  g_autofree char *token = NULL;
  g_autofree char *icon_format = NULL;
  g_autofree char *icon_size = NULL;
  g_autoptr(GVariant) icon_v = NULL;
  guint response = 2;

  /* Don't enforce app ID requirements on unsandboxed apps if the app ID
   * couldn't be determined. Otherwise the check would fail if for example the
   * app was launched from the CLI:
   * https://github.com/flatpak/xdg-desktop-portal/pull/719#issuecomment-1057412221
   */
  if (xdp_app_info_is_host (call->app_info) && g_str_equal (app_id, ""))
    {
      response = 0;
    }
  else if (!xdp_dbus_impl_dynamic_launcher_call_request_install_token_sync (impl,
                                                                            app_id,
                                                                            arg_options,
                                                                            &response,
                                                                            NULL, /* cancellable */
                                                                            &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
      response = 2;
    }

  if (response == 0)
    {
      /* Do some validation on the icon before saving it */
      icon_v = g_variant_get_variant (arg_icon_v);
      if (!icon_v || !xdp_validate_serialized_icon (icon_v, TRUE /* bytes_only */,
                                                    &icon_format, &icon_size))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 _("Dynamic launcher icon failed validation"));
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      launcher_data = g_variant_new ("(svss)", arg_name, icon_v, icon_format, icon_size);
      token = g_uuid_string_random ();

      /* Save the token in memory and return it to the caller */
      set_launcher_data_for_token (token, launcher_data);

      xdp_dbus_dynamic_launcher_complete_request_install_token (object, invocation, token);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             _("RequestInstallToken() not allowed for app id %s"),
                                             xdp_app_info_get_id (call->app_info));
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_uninstall (XdpDbusDynamicLauncher *object,
                  GDBusMethodInvocation  *invocation,
                  const gchar            *arg_desktop_file_id,
                  GVariant               *arg_options)
{
  Call *call = call_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (call->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(GError) desktop_file_error = NULL;
  g_autofree char *icon_dir = NULL;
  g_autofree char *icon_path = NULL;
  g_autofree char *desktop_dir = NULL;
  g_autoptr(GFile) icon_file = NULL;
  g_autoptr(GFile) desktop_file = NULL;
  g_autoptr(GFile) link_file = NULL;
  g_autoptr(GKeyFile) desktop_keyfile = NULL;

  if (!validate_desktop_file_id (app_id, arg_desktop_file_id, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  icon_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_ICONS_DIR, NULL);
  desktop_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_APPLICATIONS_DIR, NULL);

  link_file = g_file_new_build_filename (g_get_user_data_dir (), "applications", arg_desktop_file_id, NULL);
  if (!g_file_delete (link_file, NULL, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_info ("Uninstall() method failed because launcher '%s' does not exist", arg_desktop_file_id);
      goto error;
    }

  desktop_file = g_file_new_build_filename (desktop_dir, arg_desktop_file_id, NULL);
  desktop_keyfile = g_key_file_new ();
  if (g_key_file_load_from_file (desktop_keyfile, g_file_peek_path (desktop_file), G_KEY_FILE_NONE, NULL))
    icon_path = g_key_file_get_string (desktop_keyfile, G_KEY_FILE_DESKTOP_GROUP, "Icon", NULL);

  g_file_delete (desktop_file, NULL, &desktop_file_error);

  if (icon_path && g_str_has_prefix (icon_path, icon_dir))
    {
      icon_file = g_file_new_for_path (icon_path);
      if (!g_file_delete (icon_file, NULL, &error))
        goto error;
    }

  if (desktop_file_error)
    goto error;

  xdp_dbus_dynamic_launcher_complete_uninstall (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;

error:
  g_dbus_method_invocation_return_gerror (invocation, error);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_desktop_entry (XdpDbusDynamicLauncher *object,
                          GDBusMethodInvocation  *invocation,
                          const gchar            *arg_desktop_file_id)
{
  Call *call = call_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (call->app_info);
  g_autoptr(GError) error = NULL;
  g_autofree char *desktop_dir = NULL;
  g_autofree char *contents = NULL;
  g_autofree char *desktop_path = NULL;
  gsize length;

  if (!validate_desktop_file_id (app_id, arg_desktop_file_id, &error))
    goto error;

  desktop_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_APPLICATIONS_DIR, NULL);

  desktop_path = g_build_filename (desktop_dir, arg_desktop_file_id, NULL);
  if (!g_file_get_contents (desktop_path, &contents, &length, &error))
    goto error;
  if (length > MAX_DESKTOP_SIZE_BYTES)
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("Desktop file exceeds max size (%d): %s"),
                   MAX_DESKTOP_SIZE_BYTES, arg_desktop_file_id);
      goto error;
    }

  xdp_dbus_dynamic_launcher_complete_get_desktop_entry (object, invocation, contents);
  return G_DBUS_METHOD_INVOCATION_HANDLED;

error:
  g_dbus_method_invocation_return_gerror (invocation, error);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_icon (XdpDbusDynamicLauncher *object,
                 GDBusMethodInvocation  *invocation,
                 const gchar            *arg_desktop_file_id)
{
  Call *call = call_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (call->app_info);
  g_autoptr(GError) error = NULL;
  g_autofree char *desktop_dir = NULL;
  g_autofree char *contents = NULL;
  g_autofree char *desktop_path = NULL;
  g_autofree char *icon_dir = NULL;
  g_autofree char *icon_path = NULL;
  gsize length;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autoptr(GFile) icon_file = NULL;
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GInputStream) stream = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GIcon) bytes_icon = NULL;
  g_autoptr(GVariant) icon_v = NULL;
  const gchar *icon_format = NULL;
  int icon_size = 0;

  if (!validate_desktop_file_id (app_id, arg_desktop_file_id, &error))
    goto error;

  desktop_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_APPLICATIONS_DIR, NULL);
  icon_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_ICONS_DIR, NULL);

  desktop_path = g_build_filename (desktop_dir, arg_desktop_file_id, NULL);
  if (!g_file_get_contents (desktop_path, &contents, &length, &error))
    goto error;
  if (length > MAX_DESKTOP_SIZE_BYTES)
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("Desktop file exceeds max size (%d): %s"),
                   MAX_DESKTOP_SIZE_BYTES, arg_desktop_file_id);
      goto error;
    }

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_data (key_file, contents, -1, G_KEY_FILE_NONE, &error))
    goto error;

  icon_path = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "Icon", NULL);
  if (icon_path && g_str_has_prefix (icon_path, icon_dir))
    {
      g_autofree char *icon_dir = NULL;
      g_autofree char *icon_dir_basename = NULL;
      const char *x;

      if (g_str_has_suffix (icon_path, ".png"))
          icon_format = "png";
      else if (g_str_has_suffix (icon_path, ".svg"))
          icon_format = "svg";
      else if (g_str_has_suffix (icon_path, ".jpeg") || g_str_has_suffix (icon_path, ".jpg"))
          icon_format = "jpeg";

      /* dir should be either scalable or e.g. 512x512 */
      icon_dir = g_path_get_dirname (icon_path);
      icon_dir_basename = g_path_get_basename (icon_dir);
      if (g_strcmp0 (icon_dir_basename, "scalable") == 0) {
          /* An svg can have a width and height set, but it is probably not
           * needed since it can be scaled to any size.
           */
          icon_size = 4096;
      } else if ((x = strchr (icon_dir_basename, 'x')) != NULL) {
          icon_size = atoi (x + 1);
      }
    }

  if (!icon_format || icon_size <= 0 || icon_size > 4096)
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("Desktop file '%s' icon at unrecognized path"), arg_desktop_file_id);
      goto error;
    }

  icon_file = g_file_new_for_path (icon_path);
  icon = g_file_icon_new (icon_file);
  stream = g_loadable_icon_load (G_LOADABLE_ICON (icon), 0, NULL, NULL, NULL);

  /* Icons are usually smaller than 1 MiB. Set a 10 MiB
   * limit so we can't use a huge amount of memory or hit
   * the D-Bus message size limit
   */
  if (stream)
      bytes = g_input_stream_read_bytes (stream, 10485760 /* 10 MiB */, NULL, NULL);
  if (bytes)
      bytes_icon = g_bytes_icon_new (bytes);
  if (bytes_icon)
      icon_v = g_icon_serialize (bytes_icon);

  if (icon_v == NULL)
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("Desktop file '%s' icon failed to serialize"), arg_desktop_file_id);
      goto error;
    }

  xdp_dbus_dynamic_launcher_complete_get_icon (object, invocation,
                                               g_variant_new_variant (icon_v),
                                               icon_format, icon_size);
  return G_DBUS_METHOD_INVOCATION_HANDLED;

error:
  g_dbus_method_invocation_return_gerror (invocation, error);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_launch (XdpDbusDynamicLauncher *object,
               GDBusMethodInvocation  *invocation,
               const gchar            *arg_desktop_file_id,
               GVariant               *arg_options)
{
  Call *call = call_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (call->app_info);
  g_autoptr(GError) error = NULL;
  g_autofree char *desktop_dir = NULL;
  g_autofree char *desktop_path = NULL;
  const char *activation_token = NULL;
  g_autoptr(GAppLaunchContext) launch_context = NULL;
  g_autoptr(GDesktopAppInfo) app_info = NULL;

  if (!validate_desktop_file_id (app_id, arg_desktop_file_id, &error))
    goto error;

  desktop_dir = g_build_filename (g_get_user_data_dir (), XDG_PORTAL_APPLICATIONS_DIR, NULL);

  desktop_path = g_build_filename (desktop_dir, arg_desktop_file_id, NULL);
  if (!g_file_test (desktop_path, G_FILE_TEST_EXISTS))
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   _("No dynamic launcher exists with id '%s'"), arg_desktop_file_id);
      goto error;
    }

  /* Unset env var set in main() */
  launch_context = g_app_launch_context_new ();
  g_app_launch_context_unsetenv (launch_context, "GIO_USE_VFS");

  /* Set activation token for focus stealing prevention */
  g_variant_lookup (arg_options, "activation_token", "&s", &activation_token);
  if (activation_token)
    g_app_launch_context_setenv (launch_context, "XDG_ACTIVATION_TOKEN", activation_token);

  app_info = g_desktop_app_info_new_from_filename (desktop_path);
  if (app_info == NULL)
    {
      g_set_error (&error,
                   XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_FAILED,
                   _("Failed to create GDesktopAppInfo for launcher with id '%s'"),
                   arg_desktop_file_id);
      goto error;
    }

  g_debug ("Launching %s", arg_desktop_file_id);
  if (!g_app_info_launch (G_APP_INFO (app_info), NULL, launch_context, &error))
    goto error;

  xdp_dbus_dynamic_launcher_complete_launch (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;

error:
  g_dbus_method_invocation_return_gerror (invocation, error);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
dynamic_launcher_iface_init (XdpDbusDynamicLauncherIface *iface)
{
  iface->handle_install = handle_install;
  iface->handle_prepare_install = handle_prepare_install;
  iface->handle_request_install_token = handle_request_install_token;
  iface->handle_uninstall = handle_uninstall;
  iface->handle_get_desktop_entry = handle_get_desktop_entry;
  iface->handle_get_icon = handle_get_icon;
  iface->handle_launch = handle_launch;
}

static void
dynamic_launcher_init (DynamicLauncher *dl)
{
  xdp_dbus_dynamic_launcher_set_version (XDP_DBUS_DYNAMIC_LAUNCHER (dl), 1);
  g_object_bind_property (G_OBJECT (impl), "supported-launcher-types",
                          G_OBJECT (dl), "supported-launcher-types",
                          G_BINDING_SYNC_CREATE);
}

static void
dynamic_launcher_class_init (DynamicLauncherClass *klass)
{
}

GDBusInterfaceSkeleton *
dynamic_launcher_create (GDBusConnection *connection,
                         const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_dynamic_launcher_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        dbus_name,
                                                        DESKTOP_PORTAL_OBJECT_PATH,
                                                        NULL,
                                                        &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create dynamic_launcher proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  dynamic_launcher = g_object_new (dynamic_launcher_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (dynamic_launcher);
}
