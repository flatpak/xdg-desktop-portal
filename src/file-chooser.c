/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
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

#include "file-chooser.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _FileChooser FileChooser;
typedef struct _FileChooserClass FileChooserClass;

struct _FileChooser
{
  XdpDbusFileChooserSkeleton parent_instance;
};

struct _FileChooserClass
{
  XdpDbusFileChooserSkeletonClass parent_class;
};

static XdpDbusImplLockdown *lockdown;
static XdpDbusImplFileChooser *impl;
static FileChooser *file_chooser;

GType file_chooser_get_type (void) G_GNUC_CONST;
static void file_chooser_iface_init (XdpDbusFileChooserIface *iface);

G_DEFINE_TYPE_WITH_CODE (FileChooser, file_chooser,
                         XDP_DBUS_TYPE_FILE_CHOOSER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_FILE_CHOOSER,
                                                file_chooser_iface_init));

static void
send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  Request *request = task_data;
  GVariantBuilder results;
  GVariantBuilder ruris;
  guint response;
  GVariant *options;
  DocumentFlags flags = DOCUMENT_FLAG_WRITABLE | DOCUMENT_FLAG_DIRECTORY;
  g_autofree char **uris = NULL;
  GVariant *choices;
  GVariant *current_filter;
  GVariant *writable;

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&ruris, G_VARIANT_TYPE_STRING_ARRAY);

  REQUEST_AUTOLOCK (request);

  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "for-save")) == TRUE)
    flags |= DOCUMENT_FLAG_FOR_SAVE;
  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "directory")) == FALSE)
    flags &= ~DOCUMENT_FLAG_DIRECTORY;
  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  if (response != 0)
    goto out;

  writable = g_variant_lookup_value (options, "writable", G_VARIANT_TYPE("b"));
  if (writable && !g_variant_get_boolean (writable))
    flags &= ~DOCUMENT_FLAG_WRITABLE;

  choices = g_variant_lookup_value (options, "choices", G_VARIANT_TYPE ("a(ss)"));
  if (choices)
    g_variant_builder_add (&results, "{sv}", "choices", choices);

  current_filter = g_variant_lookup_value (options, "current_filter", G_VARIANT_TYPE ("(sa(us))"));
  if (current_filter)
    g_variant_builder_add (&results, "{sv}", "current_filter", current_filter);

  if (g_variant_lookup (options, "uris", "^a&s", &uris))
    {
      int i;

      for (i = 0; uris && uris[i]; i++)
        {
          g_autofree char *ruri = NULL;
          g_autoptr(GError) error = NULL;

          g_assert (uris[i] != NULL);

          if (!g_str_has_prefix (uris[i], "file://"))
            {
              g_warning ("Only URIs with the \"file://\" scheme are allowed");
              continue;
            }

          if (xdp_app_info_is_host (request->app_info))
            ruri = g_strdup (uris[i]);
          else
            ruri = register_document (uris[i], xdp_app_info_get_id (request->app_info), flags, &error);

          if (ruri == NULL)
            {
              g_warning ("Failed to register %s: %s", uris[i], error->message);
              continue;
            }
          g_debug ("convert uri %s -> %s\n", uris[i], ruri);
          g_variant_builder_add (&ruris, "s", ruri);
        }
    }

out:
  g_variant_builder_add (&results, "{sv}", "uris", g_variant_builder_end (&ruris));

  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&results));
      request_unexport (request);
    }

  g_task_return_boolean (task, TRUE);
}

/* Calling Lookup on a nonexisting path does not work, so we
 * pull the doc id out of the path manually.
 */
static gboolean
looks_like_document_portal_path (const char *path,
                                 char **guessed_docid)
{
  const char *prefix = "/run/user/";
  char *docid;
  char *p, *q;

  if (!g_str_has_prefix (path, prefix))
    return FALSE;

  p = strstr (path, "/doc/");
  if (!p)
    return FALSE;

  p += strlen ("/doc/");
  q = strchr (p, '/');
  if (q)
    docid = g_strndup (p, q - p);
  else
    docid = g_strdup (p);

  if (docid[0] == '\0')
    {
      g_free (docid);
      return FALSE;
    }

  *guessed_docid = docid;
  return TRUE;
}

static char *
get_host_folder_for_doc_id (const char *doc_id)
{
  g_autofree char *real_path = get_real_path_for_doc_id (doc_id);
  g_autofree char *host_folder = NULL;

  if (real_path != NULL)
    host_folder = g_path_get_dirname (real_path);

  return g_steal_pointer (&host_folder);
}

static void
open_file_done (GObject *source,
                GAsyncResult *result,
                gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_file_chooser_call_open_file_finish (XDP_DBUS_IMPL_FILE_CHOOSER (source),
                                                         &response,
                                                         &options,
                                                         result,
                                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
check_value_type (const char *key,
                  GVariant *value,
                  const GVariantType *type,
                  GError **error)
{
  if (g_variant_is_of_type (value, type))
    return TRUE;

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "expected type for key %s is %s, found %s",
               key, (const char *)type, (const char *)g_variant_get_type (value));

  return FALSE;
}

static gboolean
check_filter (GVariant *filter,
              GError **error)
{
  const char *name;
  g_autoptr(GVariant) list = NULL;
  int i;

  g_variant_get (filter, "(&s@a(us))", &name, &list);

  if (name[0] == 0)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "name is empty");
      return FALSE;
    }

  if (g_variant_n_children (list) == 0)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "no filters");
      return FALSE;
    }

  for (i = 0; i < g_variant_n_children (list); i++)
    {
      guint32 type;
      const char *string;

      g_variant_get_child (list, i, "(u&s)", &type, &string);
      if (type == 0)
        {
          /* TODO: validate glob */
          if (string[0] == 0)
            {
              g_set_error_literal (error,
                                   XDG_DESKTOP_PORTAL_ERROR,
                                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                   "invalid glob pattern");
              return FALSE;
            }
        }
      else if (type == 1)
        {
          /* TODO: validate content type */
          if (string[0] == 0)
            {
              g_set_error_literal (error,
                                   XDG_DESKTOP_PORTAL_ERROR,
                                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                   "invalid content type");
              return FALSE;
            }
        }
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "invalid filter type: %u", type);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
validate_filters (const char *key,
                  GVariant *value,
                  GVariant *options,
                  GError **error)
{
  gsize i;

  if (!check_value_type ("filters", value, G_VARIANT_TYPE ("a(sa(us))"), error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (value); i++)
    {
      g_autoptr(GVariant) filter = g_variant_get_child_value (value, i);

      if (!check_filter (filter, error))
        {
          g_prefix_error (error, "invalid filter: ");
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
validate_current_filter (const char *key,
                         GVariant *value,
                         GVariant *options,
                         GError **error)
{
  g_autoptr(GVariant) filters = NULL;
  gsize i, n_children;

  if (!check_value_type ("current_filter", value, G_VARIANT_TYPE ("(sa(us))"), error))
    return FALSE;

  if (!check_filter (value, error))
    {
      g_prefix_error (error, "invalid filter: ");
      return FALSE;
    }

  /* If the filters list is nonempty and current_filter is specified,
   * then the list must contain current_filter. But if the list is
   * empty, current_filter may be anything.
   */
  filters = g_variant_lookup_value (options, "filters", G_VARIANT_TYPE ("a(sa(us))"));
  if (!filters)
    return TRUE;

  if (!check_value_type ("filters", filters, G_VARIANT_TYPE ("a(sa(us))"), error))
    {
      g_prefix_error (error, "filters list is invalid: ");
      return FALSE;
    }

  n_children = g_variant_n_children (filters);
  if (n_children == 0)
    return TRUE;

  for (i = 0; i < n_children; i++)
    {
      g_autoptr(GVariant) filter = g_variant_get_child_value (filters, i);
      if (g_variant_equal (filter, value))
        return TRUE;
    }

  g_set_error_literal (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "current_filter, if specified, must be present in filters list if list is nonempty");
  return FALSE;
}

static gboolean
check_choice (GVariant *choice,
              GError **error)
{
  const char *id;
  const char *label;
  g_autoptr(GVariant) options = NULL;
  const char *option;
  int i;
  gboolean seen_option;

  g_variant_get (choice, "(&s&s@a(ss)&s)", &id, &label, &options, &option);

  if (id[0] == 0)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "id is empty");
      return FALSE;
    }

  if (label[0] == 0)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "label is empty");
      return FALSE;
    }

  if (g_variant_n_children (options) == 0)
    {
      const char *values[] = { "", "true", "false", NULL };
      if (!g_strv_contains (values, option))
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "bad current option: %s", option);
          return FALSE;
        }

      return TRUE;
    }

  seen_option = FALSE;
  for (i = 0; i < g_variant_n_children (options); i++)
    {
      const char *o_id;
      const char *o_label;

      g_variant_get_child (options, i, "(&s&s)", &o_id, &o_label);

      if (o_id[0] == 0)
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "option id is empty");
          return FALSE;
        }
      if (o_label[0] == 0)
        {
          g_set_error_literal (error,
                               XDG_DESKTOP_PORTAL_ERROR,
                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                               "option label is empty");
          return FALSE;
        }

      if (strcmp (o_id, option) == 0)
        seen_option = TRUE;
    }

  if (!seen_option && option[0] != 0)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "bad current option: %s", option);
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_choices (const char *key,
                  GVariant *value,
                  GVariant *options,
                  GError **error)
{
  int i;

  if (!check_value_type ("choices", value, G_VARIANT_TYPE ("a(ssa(ss)s)"), error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (value); i++)
    {
      g_autoptr(GVariant) choice = g_variant_get_child_value (value, i);

      if (!check_choice (choice, error))
        {
          g_prefix_error (error, "invalid choice: ");
          return FALSE;
        }
    }

  return TRUE;
}

/* Note: current_folder is intentionally left out */
static XdpOptionKey open_file_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING, NULL },
  { "modal", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "multiple", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "directory", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "filters", (const GVariantType *)"a(sa(us))", validate_filters },
  { "current_filter", (const GVariantType *)"(sa(us))", validate_current_filter },
  { "choices", (const GVariantType *)"a(ssa(ss)s)", validate_choices },
};

static gboolean
handle_open_file (XdpDbusFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariantBuilder options;
  g_autoptr(GVariant) dir_option = NULL;

  g_debug ("Handling OpenFile");

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options,
                           open_file_options, G_N_ELEMENTS (open_file_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_autoptr(GVariant) value =
      g_variant_lookup_value (arg_options, "current_folder", G_VARIANT_TYPE_BYTESTRING);

    if (value)
      {
        const char *path_from_app = g_variant_get_bytestring (value);
        g_autofree char *host_path = g_strdup (path_from_app);
        g_autofree char *doc_id_from_app = NULL;
        if (looks_like_document_portal_path (host_path, &doc_id_from_app))
          {
            char *real_path = get_host_folder_for_doc_id (doc_id_from_app);
            if (real_path)
              {
                g_free (host_path);
                host_path = real_path;
              }
            g_debug ("OpenFile: translating current_folder value '%s' to host path '%s'", path_from_app, host_path);
          }
        g_variant_builder_add (&options, "{sv}", "current_folder",
                              g_variant_new_bytestring (host_path));
      }
  }

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  dir_option = g_variant_lookup_value (arg_options,
                                       "directory",
                                       G_VARIANT_TYPE_BOOLEAN);
  if (dir_option && g_variant_get_boolean (dir_option))
    g_object_set_data (G_OBJECT (request), "directory", GINT_TO_POINTER (TRUE));

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_impl_file_chooser_call_open_file (impl,
                                             request->id,
                                             app_id,
                                             arg_parent_window,
                                             arg_title,
                                             g_variant_builder_end (&options),
                                             NULL,
                                             open_file_done,
                                             g_object_ref (request));

  xdp_dbus_file_chooser_complete_open_file (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/* Note that current_file and current_folder are intentionally left out here.
 * It is handled separately below
 */
static XdpOptionKey save_file_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING, NULL },
  { "modal", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "filters", (const GVariantType *)"a(sa(us))", validate_filters },
  { "current_filter", (const GVariantType *)"(sa(us))", validate_current_filter },
  { "current_name", G_VARIANT_TYPE_STRING, NULL },
  { "choices", (const GVariantType *)"a(ssa(ss)s)", validate_choices }
};

static void
save_file_done (GObject *source,
                GAsyncResult *result,
                gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_file_chooser_call_save_file_finish (XDP_DBUS_IMPL_FILE_CHOOSER (source),
                                                         &response,
                                                         &options,
                                                         result,
                                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
handle_save_file (XdpDbusFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  XdpDbusImplRequest *impl_request;
  GVariantBuilder options;

  g_debug ("Handling SaveFile");

  if (xdp_dbus_impl_lockdown_get_disable_save_to_disk (lockdown))
    {
      g_debug ("File saving disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "File saving disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options,
                           save_file_options, G_N_ELEMENTS (save_file_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_autoptr(GVariant) value = g_variant_lookup_value (arg_options,
                                                        "current_file",
                                                        G_VARIANT_TYPE_BYTESTRING);

    if (value)
      {
        const char *path = g_variant_get_bytestring (value);
        g_autofree char *host_path = get_real_path_for_doc_path (path, request->app_info);
        g_autofree char *doc_id = NULL;

        if (strcmp (path, host_path) == 0 &&
            looks_like_document_portal_path (path, &doc_id))
          {
            char *real_path = get_real_path_for_doc_id (doc_id);

            if (real_path)
              {
                g_free (host_path);
                host_path = real_path;
              }
          }

        g_debug ("SaveFile: translating current_file value '%s' to host path '%s'", path, host_path);

        g_variant_builder_add (&options, "{sv}", "current_file", g_variant_new_bytestring (host_path));
      }
  }
  {
      g_autoptr(GVariant) value =
        g_variant_lookup_value (arg_options, "current_folder", G_VARIANT_TYPE_BYTESTRING);

      if (value)
        {
          const char *path_from_app = g_variant_get_bytestring (value);
          g_autofree char *host_path = g_strdup (path_from_app);
          g_autofree char *doc_id_from_app = NULL;
          if (looks_like_document_portal_path (host_path, &doc_id_from_app))
            {
              char *real_path = get_host_folder_for_doc_id (doc_id_from_app);
              if (real_path)
                {
                  g_free (host_path);
                  host_path = real_path;
                }
              g_debug ("SaveFile: translating current_folder value '%s' to host path '%s'", path_from_app, host_path);
            }
          g_variant_builder_add (&options, "{sv}", "current_folder",
                                g_variant_new_bytestring (host_path));
        }
  }

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data (G_OBJECT (request), "for-save", GINT_TO_POINTER (TRUE));

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_impl_file_chooser_call_save_file (impl,
                                             request->id,
                                             app_id,
                                             arg_parent_window,
                                             arg_title,
                                             g_variant_builder_end (&options),
                                             NULL,
                                             save_file_done,
                                             g_object_ref (request));

  xdp_dbus_file_chooser_complete_save_file (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static XdpOptionKey save_files_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING, NULL },
  { "modal", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "current_name", G_VARIANT_TYPE_STRING, NULL },
  { "current_folder", G_VARIANT_TYPE_BYTESTRING, NULL },
  { "files", G_VARIANT_TYPE_BYTESTRING_ARRAY, NULL },
  { "choices", (const GVariantType *)"a(ssa(ss)s)", validate_choices  }
};

static void
save_files_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_file_chooser_call_save_files_finish (XDP_DBUS_IMPL_FILE_CHOOSER (source),
                                                          &response,
                                                          &options,
                                                          result,
                                                          &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
handle_save_files (XdpDbusFileChooser *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   const gchar *arg_title,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  XdpDbusImplRequest *impl_request;
  GVariantBuilder options;

  if (xdp_dbus_impl_lockdown_get_disable_save_to_disk (lockdown))
    {
      g_debug ("File saving disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "File saving disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  if (!xdp_filter_options (arg_options, &options,
                           save_files_options, G_N_ELEMENTS (save_files_options),
                           &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  impl_request =
    xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                          request->id,
                                          NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set_data (G_OBJECT (request), "for-save", GINT_TO_POINTER (TRUE));

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_impl_file_chooser_call_save_files (impl,
                                              request->id,
                                              app_id,
                                              arg_parent_window,
                                              arg_title,
                                              g_variant_builder_end (&options),
                                              NULL,
                                              save_files_done,
                                              g_object_ref (request));

  xdp_dbus_file_chooser_complete_save_files (object, invocation, request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
file_chooser_iface_init (XdpDbusFileChooserIface *iface)
{
  iface->handle_open_file = handle_open_file;
  iface->handle_save_file = handle_save_file;
  iface->handle_save_files = handle_save_files;
}

static void
file_chooser_init (FileChooser *fc)
{
  xdp_dbus_file_chooser_set_version (XDP_DBUS_FILE_CHOOSER (fc), 4);
}

static void
file_chooser_class_init (FileChooserClass *klass)
{
}

GDBusInterfaceSkeleton *
file_chooser_create (GDBusConnection *connection,
                     const char      *dbus_name,
                     gpointer         lockdown_proxy)
{
  g_autoptr(GError) error = NULL;

  lockdown = lockdown_proxy;

  impl = xdp_dbus_impl_file_chooser_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    dbus_name,
                                                    DESKTOP_PORTAL_OBJECT_PATH,
                                                    NULL,
                                                    &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create file chooser proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  file_chooser = g_object_new (file_chooser_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (file_chooser);
}
