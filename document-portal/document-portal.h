/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <sys/stat.h>

#include <gio/gio.h>

#include "document-enums.h"
#include "xdp-types.h"

typedef enum {
      VALIDATE_FD_FILE_TYPE_REGULAR,
      VALIDATE_FD_FILE_TYPE_DIR,
      VALIDATE_FD_FILE_TYPE_ANY,
} ValidateFdType;

gboolean validate_fd (int           fd,
                      XdpAppInfo   *app_info,
                      ValidateFdType ensure_type,
                      struct stat  *st_buf,
                      struct stat  *real_parent_st_buf,
                      GBytes      **real_dir_handle_out,
                      char        **path_out,
                      gboolean     *writable_out,
                      GError      **error);

char ** document_add_full (int                      *fd,
                           int                      *parent_dev,
                           int                      *parent_ino,
                           DocumentAddFullFlags     *documents_flags,
                           int                       n_args,
                           XdpAppInfo               *app_info,
                           const char               *target_app_id,
                           DocumentPermissionFlags   target_perms,
                           GError                  **error);
