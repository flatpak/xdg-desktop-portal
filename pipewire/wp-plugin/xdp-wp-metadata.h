/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <wp/wp.h>

#define XDP_WP_TYPE_METADATA (xdp_wp_metadata_get_type())
G_DECLARE_FINAL_TYPE (XdpWpMetadata, xdp_wp_metadata, XDP_WP, METADATA, GObject);

void xdp_wp_metadata_new (WpCore              *core,
                          WpObjectManager     *camera_om,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data);

XdpWpMetadata * xdp_wp_metadata_new_finish (GObject       *object,
                                            GAsyncResult  *res,
                                            GError       **error);
