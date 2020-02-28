/*
 * Copyright © 2018 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <gio/gio.h>
#include "document-enums.h"

gboolean validate_fd (int           fd,
                      XdpAppInfo   *app_info,
                      gboolean      is_directory,
                      struct stat  *st_buf,
                      struct stat  *real_parent_st_buf,
                      char        **path_out,
                      gboolean     *writable_out,
                      GError      **error);

char ** document_add_full (int                      *fd,
                           int                      *parent_dev,
                           int                      *parent_ino,
                           int                       n_args,
                           DocumentAddFullFlags      flags,
                           XdpAppInfo               *app_info,
                           const char               *target_app_id,
                           DocumentPermissionFlags   target_perms,
                           GError                  **error);
