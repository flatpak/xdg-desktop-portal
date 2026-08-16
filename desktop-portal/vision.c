/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "vision.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

typedef struct _Vision Vision;
typedef struct _VisionClass VisionClass;

struct _Vision
{
  XdpDbusVisionSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplVision *impl;
  XdpSessionDexStore *sessions;
};

struct _VisionClass
{
  XdpDbusVisionSkeletonClass parent_class;
};

GType vision_get_type (void);

static void vision_iface_init (XdpDbusVisionIface *iface);

G_DEFINE_TYPE_WITH_CODE (Vision, vision, XDP_DBUS_TYPE_VISION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_VISION,
                                                vision_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Vision, g_object_unref)

static const char * const vision_use_cases[] = {
  "vision.describe",
  "vision.ocr",
  "vision.detect",
  "vision.segment",
  "vision.depth",
  NULL,
};

static const char * const vision_describe_use_cases[] = {
  "vision.describe",
  NULL,
};

static const char * const vision_ocr_use_cases[] = {
  "vision.ocr",
  NULL,
};

static const char * const vision_detect_use_cases[] = {
  "vision.detect",
  NULL,
};

static const char * const vision_segment_use_cases[] = {
  "vision.segment",
  NULL,
};

static const char * const vision_depth_use_cases[] = {
  "vision.depth",
  NULL,
};

static void
forward_vision_text_received (XdpDbusImplVision *impl G_GNUC_UNUSED,
                              const char        *request_handle,
                              const char        *session_handle,
                              const char        *text,
                              gboolean           done,
                              ModelRequest      *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "VisionTextReceived",
                             g_variant_new ("(oosb)",
                                            request_handle,
                                            session_handle,
                                            text,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_vision_detections_received (XdpDbusImplVision *impl G_GNUC_UNUSED,
                                    const char        *request_handle,
                                    const char        *session_handle,
                                    GVariant          *detections,
                                    gboolean           done,
                                    ModelRequest      *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "VisionDetectionsReceived",
                             g_variant_new ("(oo@a(sddddd)b)",
                                            request_handle,
                                            session_handle,
                                            detections,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_vision_masks_received (XdpDbusImplVision *impl G_GNUC_UNUSED,
                               const char        *request_handle,
                               const char        *session_handle,
                               GVariant          *masks,
                               gboolean           done,
                               ModelRequest      *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "VisionMasksReceived",
                             g_variant_new ("(oo@a(sdddddsii)b)",
                                            request_handle,
                                            session_handle,
                                            masks,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_vision_depth_received (XdpDbusImplVision *impl G_GNUC_UNUSED,
                               const char        *request_handle,
                               const char        *session_handle,
                               GVariant          *depth,
                               gboolean           done,
                               ModelRequest      *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "VisionDepthReceived",
                             g_variant_new ("(oo@(iiadsdd)b)",
                                            request_handle,
                                            session_handle,
                                            depth,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static gboolean
handle_vision_get_use_case_availability (XdpDbusVision        *object,
                                         GDBusMethodInvocation *invocation,
                                         const char            *arg_use_case,
                                         GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!model_availability_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_use_case_is_supported (arg_use_case, vision_use_cases))
    {
      availability = model_unsupported_use_case_availability (arg_use_case);

      xdp_dbus_vision_complete_get_use_case_availability (
        object,
        invocation,
        availability);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  availability = model_get_use_case_availability (
    G_DBUS_PROXY (vision->impl),
    VISION_DBUS_IMPL_IFACE,
    xdp_app_info_get_id (app_info),
    arg_use_case,
    &error);
  if (availability == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_vision_complete_get_use_case_availability (
    object,
    invocation,
    availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_create_session (XdpDbusVision        *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_parent_window,
                              const char            *arg_use_case,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;
  XdpSessionDex *dex_session;

  if (!model_validate_use_case_for_session (invocation,
                                            arg_use_case,
                                            vision_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = model_session_create (vision->context,
                                  app_info,
                                  G_DBUS_INTERFACE_SKELETON (object),
                                  G_DBUS_PROXY (vision->impl),
                                  arg_use_case,
                                  arg_options,
                                  &error);
  if (session == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  dex_session = model_session_get_session (session);
  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
                               dex_session,
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  call_future = xdp_dbus_impl_vision_call_create_session_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    xdp_app_info_get_id (app_info),
    arg_parent_window,
    arg_use_case,
    arg_instructions);
  xdp_dbus_vision_complete_create_session (object,
                                           invocation,
                                           model_request_get_handle (request));

  if (!model_request_await_call (request, g_steal_pointer (&call_future)))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (model_request_emit_session_response (request, dex_session))
    xdp_session_dex_store_take_session (vision->sessions,
                                        g_steal_pointer (&session));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_prewarm (XdpDbusVision        *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_session_handle,
                       GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;

  session = model_session_lookup (vision->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_prewarm_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  call_future = xdp_dbus_impl_vision_call_prewarm_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request));
  xdp_dbus_vision_complete_prewarm (object,
                                    invocation,
                                    model_request_get_handle (request));
  model_request_finish (request,
                        g_steal_pointer (&call_future),
                        FALSE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_describe (XdpDbusVision        *object,
                               GDBusMethodInvocation *invocation,
                               GUnixFDList           *fd_list,
                               const char            *arg_session_handle,
                               GVariant              *arg_image_fd,
                               const char            *arg_instructions,
                               GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_image_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_image = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;

  session = model_session_lookup (vision->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamDescribe",
                                      vision_describe_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_request_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_image_fd,
                      fd_list,
                      &sealed_image_fd,
                      &sealed_fd_list,
                      &sealed_image,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
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
                                "vision-text-received",
                                G_CALLBACK (forward_vision_text_received));
  call_future = xdp_dbus_impl_vision_call_stream_describe_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_image_fd,
    arg_instructions,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_image),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_vision_complete_stream_describe (
    object,
    invocation,
    NULL,
    model_request_get_handle (request));
  model_request_finish (request,
                        g_steal_pointer (&call_future),
                        TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_ocr (XdpDbusVision        *object,
                          GDBusMethodInvocation *invocation,
                          GUnixFDList           *fd_list,
                          const char            *arg_session_handle,
                          GVariant              *arg_image_fd,
                          const char            *arg_instructions,
                          GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_image_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_image = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;

  session = model_session_lookup (vision->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamOcr",
                                      vision_ocr_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_request_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_image_fd,
                      fd_list,
                      &sealed_image_fd,
                      &sealed_fd_list,
                      &sealed_image,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
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
                                "vision-text-received",
                                G_CALLBACK (forward_vision_text_received));
  call_future = xdp_dbus_impl_vision_call_stream_ocr_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_image_fd,
    arg_instructions,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_image),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_vision_complete_stream_ocr (object,
                                       invocation,
                                       NULL,
                                       model_request_get_handle (request));
  model_request_finish (request,
                        g_steal_pointer (&call_future),
                        TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_detect (XdpDbusVision        *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList           *fd_list,
                             const char            *arg_session_handle,
                             GVariant              *arg_image_fd,
                             const char            *arg_instructions,
                             GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_image_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_image = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;

  session = model_session_lookup (vision->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamDetect",
                                      vision_detect_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_request_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_image_fd,
                      fd_list,
                      &sealed_image_fd,
                      &sealed_fd_list,
                      &sealed_image,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (
    request,
    "vision-detections-received",
    G_CALLBACK (forward_vision_detections_received));
  call_future = xdp_dbus_impl_vision_call_stream_detect_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_image_fd,
    arg_instructions,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_image),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_vision_complete_stream_detect (object,
                                          invocation,
                                          NULL,
                                          model_request_get_handle (request));
  model_request_finish (request,
                        g_steal_pointer (&call_future),
                        TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_segment (XdpDbusVision        *object,
                              GDBusMethodInvocation *invocation,
                              GUnixFDList           *fd_list,
                              const char            *arg_session_handle,
                              GVariant              *arg_image_fd,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_image_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_image = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;

  session = model_session_lookup (vision->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamSegment",
                                      vision_segment_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_segment_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_image_fd,
                      fd_list,
                      &sealed_image_fd,
                      &sealed_fd_list,
                      &sealed_image,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
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
                                "vision-masks-received",
                                G_CALLBACK (forward_vision_masks_received));
  call_future = xdp_dbus_impl_vision_call_stream_segment_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_image_fd,
    arg_instructions,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_image),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_vision_complete_stream_segment (object,
                                           invocation,
                                           NULL,
                                           model_request_get_handle (request));
  model_request_finish (request,
                        g_steal_pointer (&call_future),
                        TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_depth (XdpDbusVision        *object,
                            GDBusMethodInvocation *invocation,
                            GUnixFDList           *fd_list,
                            const char            *arg_session_handle,
                            GVariant              *arg_image_fd,
                            const char            *arg_instructions,
                            GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_image_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_image = NULL;
  g_autoptr(DexFuture) call_future = NULL;
  g_autoptr(GError) error = NULL;

  session = model_session_lookup (vision->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamDepth",
                                      vision_depth_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_request_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_image_fd,
                      fd_list,
                      &sealed_image_fd,
                      &sealed_fd_list,
                      &sealed_image,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (vision->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (object),
                               G_DBUS_PROXY (vision->impl),
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
                                "vision-depth-received",
                                G_CALLBACK (forward_vision_depth_received));
  call_future = xdp_dbus_impl_vision_call_stream_depth_future (
    vision->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_image_fd,
    arg_instructions,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_image),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_vision_complete_stream_depth (object,
                                         invocation,
                                         NULL,
                                         model_request_get_handle (request));
  model_request_finish (request,
                        g_steal_pointer (&call_future),
                        TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
vision_iface_init (XdpDbusVisionIface *iface)
{
  iface->handle_get_use_case_availability = handle_vision_get_use_case_availability;
  iface->handle_create_session = handle_vision_create_session;
  iface->handle_prewarm = handle_vision_prewarm;
  iface->handle_stream_describe = handle_vision_stream_describe;
  iface->handle_stream_ocr = handle_vision_stream_ocr;
  iface->handle_stream_detect = handle_vision_stream_detect;
  iface->handle_stream_segment = handle_vision_stream_segment;
  iface->handle_stream_depth = handle_vision_stream_depth;
}

static void
vision_dispose (GObject *object)
{
  Vision *vision = (Vision *) object;

  g_clear_object (&vision->sessions);
  g_clear_object (&vision->impl);

  G_OBJECT_CLASS (vision_parent_class)->dispose (object);
}

static void
vision_init (Vision *vision)
{
  vision->sessions = model_session_store_new ();
}

static void
vision_class_init (VisionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = vision_dispose;
}

static Vision *
vision_new (XdpContext        *context,
            XdpDbusImplVision *impl)
{
  Vision *vision;

  vision = g_object_new (vision_get_type (), NULL);
  vision->context = context;
  vision->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (vision->impl), G_MAXINT);
  xdp_dbus_vision_set_version (XDP_DBUS_VISION (vision), 1);

  return vision;
}

DexFuture *
init_vision (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplVision) impl = NULL;
  g_autoptr(Vision) vision = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, VISION_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return dex_future_new_true ();

  impl = dex_await_object (xdp_dbus_impl_vision_proxy_new_future (
      connection,
      G_DBUS_PROXY_FLAGS_NONE,
      impl_config->dbus_name,
      DESKTOP_DBUS_PATH),
    &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create vision proxy: %s", error->message);
      return dex_future_new_false ();
    }

  vision = vision_new (context, impl);
  xdp_context_take_and_export_portal (
    context,
    G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&vision)),
    XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  return dex_future_new_true ();
}
