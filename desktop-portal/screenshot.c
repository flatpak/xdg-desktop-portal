/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "screenshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-documents.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-request-dex.h"
#include "xdp-utils.h"

typedef enum
{
  SCREENSHOT_TARGET_SCREEN = 1u << 0,
  SCREENSHOT_TARGET_WINDOW = 1u << 1,
  SCREENSHOT_TARGET_AREA = 1u << 2,
  SCREENSHOT_TARGET_ACTIVE_WINDOW = 1u << 3,
} ScreenshotTarget;

typedef struct _Screenshot Screenshot;
typedef struct _ScreenshotClass ScreenshotClass;

struct _Screenshot
{
  XdpDbusScreenshotSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplScreenshot *impl;
  XdpDbusImplAccess *access_impl;
  guint32 impl_version;
};

struct _ScreenshotClass
{
  XdpDbusScreenshotSkeletonClass parent_class;
};

GType screenshot_get_type (void);
static void screenshot_iface_init (XdpDbusScreenshotIface *iface);

G_DEFINE_TYPE_WITH_CODE (Screenshot, screenshot, XDP_DBUS_TYPE_SCREENSHOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SCREENSHOT,
                                                screenshot_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Screenshot, g_object_unref)

static gboolean
validate_screenshot_target (Screenshot *screenshot,
                            uint32_t    target,
                            GError    **error)
{
  uint32_t available_targets;

  switch (target)
    {
    case SCREENSHOT_TARGET_SCREEN:
    case SCREENSHOT_TARGET_WINDOW:
    case SCREENSHOT_TARGET_AREA:
    case SCREENSHOT_TARGET_ACTIVE_WINDOW:
      break;
    default:
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid screenshot target %x", target);
      return FALSE;
    }

  available_targets =
    xdp_dbus_screenshot_get_available_targets (XDP_DBUS_SCREENSHOT (screenshot));
  if (!(available_targets & target))
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Unavailable screenshot target %x", target);
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_screenshot_target_filter (const char  *key,
                                   GVariant    *value,
                                   GVariant    *options,
                                   gpointer     user_data,
                                   GError     **error)
{
  return validate_screenshot_target (user_data,
                                     g_variant_get_uint32 (value),
                                     error);
}

static XdpOptionKey screenshot_options_v3[] = {
  { "modal", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "interactive", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "target", G_VARIANT_TYPE_UINT32, validate_screenshot_target_filter }
};

static gboolean
get_permission (Screenshot     *screenshot,
                XdpRequestDex  *request,
                XdpAppInfo        *app_info,
                GVariant          *arg_options,
                const char        *arg_parent_window,
                gboolean          *permission_granted_out,
                GError           **error)
{
  XdpPermission permission;
  g_autoptr(XdpDbusImplAccessAccessDialogResult) result = NULL;
  g_autoptr(GError) local_error = NULL;

  permission = dex_await_uint (xdp_permission_get_future (
      app_info,
      SCREENSHOT_PERMISSION_TABLE,
      SCREENSHOT_PERMISSION_ID),
    &local_error);

  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (permission == XDP_PERMISSION_NO)
    {
      *permission_granted_out = FALSE;
      return TRUE;
    }

  if (permission == XDP_PERMISSION_YES)
    {
      *permission_granted_out = TRUE;
      return TRUE;
    }

  {
    g_auto(GVariantBuilder) access_opt_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
    g_autofree gchar *title = NULL;
    g_autofree gchar *subtitle = NULL;
    const gchar *body = NULL;
    gboolean modal = TRUE;
    const char *app_id = NULL;

    if (!g_variant_lookup (arg_options, "modal", "b", &modal))
      modal = TRUE;

    g_variant_builder_add (&access_opt_builder, "{sv}",
                          "deny_label", g_variant_new_string (_("Deny")));
    g_variant_builder_add (&access_opt_builder, "{sv}",
                          "grant_label", g_variant_new_string (_("Allow")));
    g_variant_builder_add (&access_opt_builder, "{sv}",
                          "icon", g_variant_new_string ("applets-screenshooter-symbolic"));
    g_variant_builder_add (&access_opt_builder, "{sv}",
                          "modal", g_variant_new_boolean (modal));

    app_id = xdp_app_info_get_id (app_info);

    if (g_strcmp0 (app_id, "") != 0)
      {
        g_autoptr(GDesktopAppInfo) info = NULL;
        g_autofree gchar *id = NULL;
        const gchar *name = NULL;

        id = g_strconcat (app_id, ".desktop", NULL);
        info = g_desktop_app_info_new (id);

        if (info)
          name = g_app_info_get_display_name (G_APP_INFO (info));
        else
          name = app_id;

        if (permission == XDP_PERMISSION_UNSET)
          {
            title = g_strdup_printf (_("Allow %s to Take Screenshots?"), name);
            subtitle = g_strdup_printf (_("%s wants to take screenshots at any time"), name);
          }
        else /* permission == XDP_PERMISSION_ASK */
          {
            title = g_strdup_printf (_("Allow %s to Take a Screenshot?"), name);
            subtitle = g_strdup_printf (_("%s wants to take a screenshot"), name);
          }
      }
    else
      {
        g_assert (xdp_app_info_is_host (app_info));
        if (permission == XDP_PERMISSION_UNSET)
          {
            /* Note: this will set the screenshot permission for all unsandboxed
            * apps for which an app ID can't be determined.
            */
            title = g_strdup (_("Allow Apps to Take Screenshots?"));
            subtitle = g_strdup (_("An app wants to take screenshots at any time"));
          }
        else /* permission == XDP_PERMISSION_ASK */
          {
            title = g_strdup (_("Allow to Take a Screenshot?"));
            subtitle = g_strdup (_("An app wants to take a screenshot"));
          }
      }

    if (permission == XDP_PERMISSION_UNSET)
      body = _("This permission can be changed at any time from the privacy settings");
    else
      body = "";

    result = dex_await_boxed (xdp_dbus_impl_access_call_access_dialog_future (
        screenshot->access_impl,
        xdp_request_dex_get_object_path (request),
        xdp_app_info_get_id (app_info),
        arg_parent_window,
        title,
        subtitle,
        body,
        g_variant_builder_end (&access_opt_builder)),
      error);
    if (!result)
      return FALSE;
  }

  if (permission == XDP_PERMISSION_UNSET)
    {
      if (!dex_await_boolean (xdp_permission_set_future (
          app_info,
          SCREENSHOT_PERMISSION_TABLE,
          SCREENSHOT_PERMISSION_ID,
          result->response == 0 ?
          XDP_PERMISSION_YES : XDP_PERMISSION_NO),
        &local_error))
        g_warning ("Setting unset permission failed: %s", local_error->message);
    }

  *permission_granted_out = (result->response == 0);
  return TRUE;
}


static gboolean
handle_screenshot (XdpDbusScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Screenshot *screenshot = (Screenshot *) object;
  g_autoptr(XdpRequestDex) request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) filtered_options = NULL;
  g_autoptr(GVariant) impl_options = NULL;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GVariantIter options_iter;
  const char *key = NULL;
  GVariant *value = NULL;
  gboolean permission_store_checked = FALSE;
  gboolean permission_granted = FALSE;
  gboolean interactive = FALSE;

  g_debug ("Handle screenshot");

  {
    g_auto(GVariantBuilder) opt_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!xdp_filter_options (arg_options,
                            &opt_builder,
                            screenshot_options_v3,
                            G_N_ELEMENTS (screenshot_options_v3),
                            screenshot,
                            &error))
      {
        g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    filtered_options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));
  }

  request = dex_await_object (xdp_request_dex_new (
      screenshot->context,
      app_info,
      G_DBUS_INTERFACE_SKELETON (object),
      G_DBUS_PROXY (screenshot->impl),
      arg_options),
    &error);

  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_screenshot_complete_screenshot (object,
                                           g_steal_pointer (&invocation),
                                           xdp_request_dex_get_object_path (request));

  if (!g_variant_lookup (filtered_options, "interactive", "b", &interactive))
    interactive = FALSE;

  if (xdp_dbus_impl_screenshot_get_version (screenshot->impl) >= 2 &&
      !interactive)
    {
      if (!get_permission (screenshot,
                          request,
                          app_info,
                          filtered_options,
                          arg_parent_window,
                          &permission_granted,
                          &error))
        {
          g_warning ("Getting permission failed: %s", error->message);

          xdp_request_dex_emit_response (request,
                                        XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                        NULL);

          if (request->exported)                                 
            xdp_request_dex_unexport (request);

          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      permission_store_checked = TRUE;

      if (!permission_granted)
        {
          xdp_request_dex_emit_response (request,
                                        XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                        NULL);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  {
    g_auto(GVariantBuilder) opt_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    g_variant_iter_init (&options_iter, filtered_options);
    while (g_variant_iter_next (&options_iter, "{&sv}", &key, &value))
      {
        g_variant_builder_add (&opt_builder, "{sv}", key, value);
        g_clear_pointer (&value, g_variant_unref);
      }

    if (permission_store_checked)
      {
        g_variant_builder_add (&opt_builder, "{sv}", "permission_store_checked",
                              g_variant_new_boolean (TRUE));
      }

    impl_options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));
  }

  g_autoptr(XdpDbusImplScreenshotScreenshotResult) result = NULL;
  XdgDesktopPortalResponseEnum response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  g_autoptr(GVariant) results = NULL;

  result = dex_await_boxed (xdp_dbus_impl_screenshot_call_screenshot_future (
      screenshot->impl,
      xdp_request_dex_get_object_path (request),
      xdp_app_info_get_id (app_info),
      arg_parent_window,
      impl_options),
    &error);

  if (result)
    {
      const char *uri;

      if (result->response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
        {
          response = result->response;
          results = g_variant_ref (result->results);
        }
      else if (!g_variant_lookup (result->results, "uri", "&s", &uri))
        {
          g_warning ("No URI was provided");
        }
      else
        {
          g_auto(GVariantBuilder) results_builder =
            G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
          g_autofree char *ruri = NULL;

          if (xdp_app_info_is_host (request->app_info))
            ruri = g_strdup (uri);
          else
            ruri = xdp_register_document (uri,
                                          xdp_app_info_get_id (request->app_info),
                                          xdp_app_info_get_gappinfo (request->app_info),
                                          XDP_DOCUMENT_FLAG_DELETABLE,
                                          &error);

          if (ruri == NULL)
            g_warning ("Failed to register %s: %s",
                       uri,
                       error ? error->message : "unknown error");
          else
            {
              g_variant_builder_add (&results_builder,
                                     "{&sv}",
                                     "uri",
                                     g_variant_new_string (ruri));
              response = result->response;
              results = g_variant_builder_end (&results_builder);
            }
        }
    }
  else
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);

      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      results = NULL;
    }

  xdp_request_dex_emit_response (request,
                                 response,
                                 results);

  if (request->exported)
    xdp_request_dex_unexport (request);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_pick_color (XdpDbusScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   GVariant *arg_options)
{
  Screenshot *screenshot = (Screenshot *) object;
  g_autoptr(XdpRequestDex) request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GError) error = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_debug ("Handle pick color");

  request = dex_await_object (xdp_request_dex_new (
      screenshot->context,
      app_info,
      G_DBUS_INTERFACE_SKELETON (object),
      G_DBUS_PROXY (screenshot->impl),
      arg_options),
    &error);

  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_screenshot_complete_pick_color (object,
                                           g_steal_pointer (&invocation),
                                           xdp_request_dex_get_object_path (request));

  
  g_autoptr(XdpDbusImplScreenshotPickColorResult) result =
    NULL;
  XdgDesktopPortalResponseEnum response =
    XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  g_autoptr(GVariant) results = NULL;

  result = dex_await_boxed (xdp_dbus_impl_screenshot_call_pick_color_future (
      screenshot->impl,
      xdp_request_dex_get_object_path (request),
      xdp_app_info_get_id (app_info),
      arg_parent_window,
      g_variant_builder_end (&opt_builder)),
    &error);
  
  if (result)
    {

      g_auto(GVariantBuilder) results_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      {
        double red, green, blue;

        if (!g_variant_lookup (result->results,
                               "color",
                               "(ddd)",
                               &red,
                               &green,
                               &blue))
          {
            g_warning ("No color was provided");
          } else {
            g_variant_builder_add (&results_builder,
                                   "{&sv}",
                                   "color",
                                   g_variant_new ("(ddd)", red, green, blue));
          }
      }
      
      response = result->response;
      results = g_variant_builder_end (&results_builder);
    }
  else
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);

      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      results = NULL;
    }
  
  xdp_request_dex_emit_response (request,
                                 response,
                                 results);

  if (request->exported)                                 
    xdp_request_dex_unexport (request);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
screenshot_iface_init (XdpDbusScreenshotIface *iface)
{
  iface->handle_screenshot = handle_screenshot;
  iface->handle_pick_color = handle_pick_color;
}

static void
screenshot_dispose (GObject *object)
{
  Screenshot *screenshot = (Screenshot *) object;

  g_clear_object (&screenshot->impl);
  g_clear_object (&screenshot->access_impl);

  G_OBJECT_CLASS (screenshot_parent_class)->dispose (object);
}

static void
screenshot_init (Screenshot *screenshot)
{
}

static void
screenshot_class_init (ScreenshotClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = screenshot_dispose;
}

static Screenshot *
screenshot_new (XdpContext *context,
                XdpDbusImplScreenshot *impl,
                XdpDbusImplAccess     *access_impl)
{
  Screenshot *screenshot;

  screenshot = g_object_new (screenshot_get_type (), NULL);
  screenshot->context = context;
  screenshot->access_impl = g_object_ref (access_impl);
  screenshot->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (screenshot->impl), G_MAXINT);


  /* Before there was a version property, the version was hardcoded to 2, so
   * make sure we retain that behaviour */
  screenshot->impl_version =
    MAX (xdp_dbus_impl_screenshot_get_version (screenshot->impl), 2);

  xdp_dbus_screenshot_set_version (XDP_DBUS_SCREENSHOT (screenshot),
                                   MIN (screenshot->impl_version, 3));

  if (screenshot->impl_version >= 3)
    {
      g_object_bind_property (G_OBJECT (screenshot->impl), "available-targets",
                              G_OBJECT (screenshot), "available-targets",
                              G_BINDING_SYNC_CREATE);
    }

  return screenshot;
}

DexFuture *
init_screenshot (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr(Screenshot) screenshot = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpDbusImplAccess *access_impl;
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplScreenshot) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SCREENSHOT_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_false ();

  access_impl = xdp_context_get_access_impl (context);
  if (access_impl == NULL)
    {
      g_warning ("The screenshot portal requires an access impl");
      return dex_future_new_false ();
    }

  impl = dex_await_object (xdp_dbus_impl_screenshot_proxy_new_future (connection,
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  impl_config->dbus_name,
                                                  DESKTOP_DBUS_PATH),
                           &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create screenshot proxy: %s", error->message);
      return dex_future_new_false ();
    }

  screenshot = screenshot_new (context, impl, access_impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&screenshot)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  return dex_future_new_true ();
}
