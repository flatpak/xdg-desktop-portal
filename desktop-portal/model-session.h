/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gunixfdlist.h>

#include "xdp-request-dex.h"
#include "xdp-sealed-fd.h"
#include "xdp-session-dex.h"

#define MODEL_TYPE_SESSION (model_session_get_type ())
G_DECLARE_FINAL_TYPE (ModelSession,
                      model_session,
                      MODEL, SESSION,
                      GObject)

XdpSessionDexStore * model_session_store_new (void);

ModelSession * model_session_create (XdpContext             *context,
                                     XdpAppInfo             *app_info,
                                     GDBusInterfaceSkeleton *portal,
                                     GDBusProxy             *impl,
                                     const char             *use_case,
                                     GVariant               *options,
                                     GError                **error);

ModelSession * model_session_lookup (XdpSessionDexStore   *store,
                                     GDBusMethodInvocation *invocation,
                                     const char            *session_handle);

XdpSessionDex * model_session_get_session (ModelSession *session);

gboolean model_use_case_is_supported (const char         *use_case,
                                      const char * const *supported_use_cases);

GVariant * model_unsupported_use_case_availability (const char *use_case);

GVariant * model_get_use_case_availability (GDBusProxy  *impl,
                                             const char  *interface_name,
                                             const char  *app_id,
                                             const char  *use_case,
                                             GError     **error);

gboolean model_validate_use_case_for_session (GDBusMethodInvocation *invocation,
                                               const char            *use_case,
                                               const char * const    *supported_use_cases);

gboolean model_session_ensure_use_case (GDBusMethodInvocation *invocation,
                                        ModelSession          *session,
                                        const char            *method,
                                        const char * const    *allowed_use_cases);

gboolean model_session_options_validate (GVariant  *options,
                                          GError   **error);

gboolean model_availability_options_validate (GVariant  *options,
                                               GError   **error);

gboolean model_prewarm_options_validate (GVariant  *options,
                                          GError   **error);

GVariant * model_response_options_from_vardict (GVariant  *options,
                                                 GError   **error);

GVariant * model_token_options_from_vardict (GVariant  *options,
                                              GError   **error);

GVariant * model_request_options_from_vardict (GVariant  *options,
                                                GError   **error);

GVariant * model_speech_options_from_vardict (GVariant  *options,
                                               GError   **error);

GVariant * model_synthesis_options_from_vardict (GVariant  *options,
                                                  GError   **error);

GVariant * model_segment_options_from_vardict (GVariant  *options,
                                                GError   **error);

gboolean model_seal_fd (GVariant      *handle,
                        GUnixFDList   *fd_list,
                        GVariant     **sealed_handle_out,
                        GUnixFDList  **sealed_fd_list_out,
                        XdpSealedFd  **sealed_fd_out,
                        GError       **error);

gboolean model_seal_fds (GVariant      *handles,
                         GUnixFDList   *fd_list,
                         GVariant     **sealed_handles_out,
                         GUnixFDList  **sealed_fd_list_out,
                         GPtrArray    **sealed_fds_out,
                         GError       **error);

typedef struct _ModelRequest ModelRequest;

ModelRequest * model_request_new (XdpContext             *context,
                                  XdpAppInfo             *app_info,
                                  GDBusInterfaceSkeleton *portal,
                                  GDBusProxy             *impl,
                                  XdpSessionDex          *session,
                                  GVariant               *options,
                                  GError                **error);

void model_request_free (ModelRequest *request);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ModelRequest, model_request_free)

const char * model_request_get_handle (ModelRequest *request);

const char * model_request_get_session_handle (ModelRequest *request);

gboolean model_request_matches (ModelRequest *request,
                                const char   *request_handle,
                                const char   *session_handle);

void model_request_connect_signal (ModelRequest *request,
                                   const char   *detailed_signal,
                                   GCallback     callback);

void model_request_connect_loading (ModelRequest *request);

void model_request_emit_signal (ModelRequest *request,
                                const char   *signal_name,
                                GVariant     *parameters);

void model_request_mark_terminal (ModelRequest *request);

void model_request_take_sealed_fd (ModelRequest *request,
                                   XdpSealedFd  *sealed_fd,
                                   GUnixFDList  *sealed_fd_list);

void model_request_take_sealed_fds (ModelRequest *request,
                                    GPtrArray    *sealed_fds,
                                    GUnixFDList  *sealed_fd_list);

gboolean model_request_await_call (ModelRequest *request,
                                   DexFuture    *call_future);

void model_request_finish (ModelRequest *request,
                           DexFuture    *call_future,
                           gboolean      wait_for_terminal);

void model_request_emit_error (ModelRequest *request,
                               GError       *error);

gboolean model_request_emit_session_response (ModelRequest  *request,
                                               XdpSessionDex *session);
