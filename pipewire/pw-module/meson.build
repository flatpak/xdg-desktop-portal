# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

xdp_pipewire_module_sources = [
  'module-portal.c',
]

xdp_pipewire_module = shared_library(
  'xdg-desktop-portal',
  xdp_pipewire_module_sources,
  dependencies: [pipewire_dep, libdbus_dep],
  install: true,
  install_dir: pipewire_module_dir,
)
