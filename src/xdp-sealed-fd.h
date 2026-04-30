/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gio.h>

#define XDP_TYPE_SEALED_FD (xdp_sealed_fd_get_type())
G_DECLARE_FINAL_TYPE (XdpSealedFd,
                      xdp_sealed_fd,
                      XDP, SEALED_FD,
                      GObject);

XdpSealedFd * xdp_sealed_fd_new_take_memfd (int           memfd,
                                            GError      **error);
XdpSealedFd * xdp_sealed_fd_new_from_bytes (GBytes       *bytes,
                                            GError      **error);
XdpSealedFd * xdp_sealed_fd_new_from_handle (GVariant     *handle,
                                             GUnixFDList  *fd_list,
                                             GError      **error);
int xdp_sealed_fd_get_fd (XdpSealedFd  *sealed_fd);
int xdp_sealed_fd_dup_fd (XdpSealedFd  *sealed_fd);

GBytes *xdp_sealed_fd_get_bytes (XdpSealedFd  *sealed_fd,
                                 GError      **error);
GVariant *xdp_sealed_fd_to_handle (XdpSealedFd  *sealed_fd,
                                   GUnixFDList  *fd_list,
                                   GError      **error);
