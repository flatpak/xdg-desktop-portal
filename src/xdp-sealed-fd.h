/*
 * Copyright © 2024 GNOME Foundation Inc.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>

#define XDP_TYPE_SEALED_FD (xdp_sealed_fd_get_type())
G_DECLARE_FINAL_TYPE (XdpSealedFd,
                      xdp_sealed_fd,
                      XDP, SEALED_FD,
                      GObject)

XdpSealedFd * xdp_sealed_fd_new_take_memfd (int           memfd,
                                            GError      **error);
XdpSealedFd * xdp_sealed_fd_new_from_bytes (GBytes       *bytes,
                                            GError      **error);
XdpSealedFd * xdp_sealed_fd_new_from_handle (GVariant     *handle,
                                             GUnixFDList  *fd_list,
                                             GError      **error);
int xdp_sealed_fd_get_fd (XdpSealedFd  *sealed_fd);
int xdp_sealed_fd_dup_fd (XdpSealedFd  *sealed_fd);

