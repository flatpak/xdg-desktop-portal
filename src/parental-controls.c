/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gio/gio.h>

#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-request.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

#include "parental-controls.h"

typedef struct _ParentalControls ParentalControls;
typedef struct _ParentalControlsClass ParentalControlsClass;

struct _ParentalControls
{
  XdpDbusParentalControlsSkeleton parent_instance;

  XdpDbusImplParentalControls *impl;
};

struct _ParentalControlsClass
{
  XdpDbusParentalControlsSkeletonClass parent_class;
};

GType parental_controls_get_type (void) G_GNUC_CONST;
static void parental_controls_iface_init (XdpDbusParentalControlsIface *iface);

G_DEFINE_TYPE_WITH_CODE (ParentalControls, parental_controls, XDP_DBUS_TYPE_PARENTAL_CONTROLS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_PARENTAL_CONTROLS,
                                                parental_controls_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ParentalControls, g_object_unref)

static void
send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  XdpRequest *request = task_data;
  guint response;
  GVariant *results;
  g_auto(GVariantBuilder) new_results =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) lowv = NULL;
  g_autoptr(GVariant) highv = NULL;

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  results = (GVariant *)g_object_get_data (G_OBJECT (request), "results");

  if (response != 0)
    goto out;

  lowv = g_variant_lookup_value (results, "low", G_VARIANT_TYPE_INT32);
  if (lowv)
    g_variant_builder_add (&new_results, "{sv}", "low", lowv);
  highv = g_variant_lookup_value (results, "high", G_VARIANT_TYPE_INT32);
  if (highv)
    g_variant_builder_add (&new_results, "{sv}", "high", highv);

out:
  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&new_results));
      xdp_request_unexport (request);
    }
}

static void
query_age_bracket_done (GObject *source,
                        GAsyncResult *result,
                        gpointer data)
{
  g_autoptr(XdpRequest) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_dbus_impl_parental_controls_call_query_age_bracket_finish (XDP_DBUS_IMPL_PARENTAL_CONTROLS (source),
                                                                      &response,
                                                                      &results,
                                                                      result,
                                                                      &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (results)
    g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static gboolean
handle_query_age_bracket (XdpDbusParentalControls *object,
                          GDBusMethodInvocation *invocation,
                          const gchar *arg_parent_window,
                          GVariant *arg_gates,
                          GVariant *arg_options)
{
  ParentalControls *pc = (ParentalControls *) object;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_auto(GVariantBuilder) options =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  g_debug ("Handling QueryAgeBracket");

  // Before merging we want some way to check that the caller has access to query this
  // Assuming https://github.com/flatpak/xdg-desktop-portal/pull/1924 goes ahead it'll look like this

  // if (!xdp_app_info_has_entitlement (app_info,
  //                                  "org.freedesktop.portal.ParentalControls.QueryAgeBracket"))
  // {
  //   g_dbus_method_invocation_return_error (invocation,
  //                                          XDG_DESKTOP_PORTAL_ERROR,
  //                                          XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
  //                                          "Missing entitlement "
  //                                          "org.freedesktop.portal.ParentalControls.QueryAgeBracket");
  //   return G_DBUS_METHOD_INVOCATION_HANDLED;
  // }

  // Or if it doesn't

  // XdpPermission permission = xdp_get_permission_sync (request->app_info,
  //                                       ACCOUNT_PERMISSION_TABLE,
  //                                       ACCOUNT_PERMISSION_PARENTAL_CONTROLS);
  // if (permission != XDP_PERMISSION_YES)
  //   {
  //     g_dbus_method_invocation_return_error (invocation,Expand commentComment on line R332
  //                                            XDG_DESKTOP_PORTAL_ERROR,
  //                                            XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
  //                                            "Permission denied");
  //     return G_DBUS_METHOD_INVOCATION_HANDLED;
  //   }


  REQUEST_AUTOLOCK (request);

  /* Validate gates: require at least two entries */
  if (g_variant_n_children (arg_gates) < 1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "At least one age gate is required");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (pc->impl)),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (G_DBUS_PROXY (pc->impl)),
    request->id,
    NULL, &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_impl_parental_controls_call_query_age_bracket (pc->impl,
                                                          request->id,
                                                          app_id,
                                                          arg_parent_window,
                                                          arg_gates,
                                                          g_variant_builder_end (&options),
                                                          NULL,
                                                          query_age_bracket_done,
                                                          g_object_ref (request));

  xdp_dbus_parental_controls_complete_query_age_bracket (object, invocation,
                                                         request->id);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
parental_controls_iface_init (XdpDbusParentalControlsIface *iface)
{
  iface->handle_query_age_bracket = handle_query_age_bracket;
}

static void
parental_controls_dispose (GObject *object)
{
  ParentalControls *pc = (ParentalControls *) object;
  g_clear_object (&pc->impl);
  G_OBJECT_CLASS (parental_controls_parent_class)->dispose (object);
}

static void
parental_controls_init (ParentalControls *pc)
{
}

static void
parental_controls_class_init (ParentalControlsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = parental_controls_dispose;
}

static ParentalControls *
parental_controls_new (XdpDbusImplParentalControls *impl)
{
  ParentalControls *pc;

  pc = g_object_new (parental_controls_get_type (), NULL);
  pc->impl = g_object_ref (impl);

  xdp_dbus_parental_controls_set_version (XDP_DBUS_PARENTAL_CONTROLS (pc), 1);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (pc->impl), G_MAXINT);

  return pc;
}

void
init_parental_controls (XdpContext *context)
{
  g_autoptr(ParentalControls) pc = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplParentalControls) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, PARENTAL_CONTROLS_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_parental_controls_proxy_new_sync (connection,
                                                         G_DBUS_PROXY_FLAGS_NONE,
                                                         impl_config->dbus_name,
                                                         DESKTOP_DBUS_PATH,
                                                         NULL,
                                                         &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create parental controls proxy: %s", error->message);
      return;
    }

  pc = parental_controls_new (impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&pc)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
