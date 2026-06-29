# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

xdp_utils_deps = []
xdp_utils_includes = include_directories('.')
xdp_utils_sources = files(
  'xdp-app-info.c',
  'xdp-app-info-flatpak.c',
  'xdp-app-info-host.c',
  'xdp-app-info-registry.c',
  'xdp-app-info-snap.c',
  'xdp-app-info-linyaps.c',
  'xdp-entitlements.c',
  'xdp-sealed-fd.c',
  'xdp-usb-query.c',
  'xdp-utils.c',
)


xdp_utils_sources += gnome.mkenums_simple(
  'xdp-enum-types',
  sources: files(
    'xdp-app-info-private.h',
  ),
)

if have_libsystemd
  xdp_utils_sources += files('sd-escape.c')
  xdp_utils_deps += [libsystemd_dep]
endif