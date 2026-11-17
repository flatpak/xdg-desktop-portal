/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <sys/stat.h>

#include <glib.h>
#include <glnx-pidref.h>

typedef struct _XdpMountNs XdpMountNs;

XdpMountNs * xdp_mount_ns_new (GLnxPidRef *pidref);

void xdp_mount_ns_free (XdpMountNs *mount_ns);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpMountNs, xdp_mount_ns_free)

char * xdp_mount_ns_remap_path (XdpMountNs  *mount_ns,
                                const char  *path,
                                struct stat *st_buf);

char * xdp_mount_ns_remap_path_unverified (XdpMountNs *mount_ns,
                                           const char *path);
