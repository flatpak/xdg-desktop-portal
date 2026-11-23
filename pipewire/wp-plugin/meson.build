# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

xdp_wireplumber_module_sources = [
  'xdp-wp-permission-manager.c',
  'xdp-wp-plugin.c',
  'xdp-wp-app-audio-capture.c',
  xdp_utils_sources,
]

xdp_wireplumber_module_sources += gnome.gdbus_codegen(
  'xdp-impl-dbus',
  sources: portal_impl_sources,
  interface_prefix: 'org.freedesktop.impl.portal',
  namespace: 'XdpDbusImpl',
  docbook: 'portal',
)

xdp_wireplumber_module = shared_library(
  'xdg-desktop-portal',
  xdp_wireplumber_module_sources,
  dependencies: [
    wireplumber_dep,
    pipewire_dep,
    gio_unix_dep,
    json_glib_dep,
    libdex_dep,
    xdp_utils_deps,
  ],
  include_directories: [common_includes, xdp_pipewire_includes, xdp_utils_includes],
  install: true,
  install_dir: wireplumber_module_dir,
)
