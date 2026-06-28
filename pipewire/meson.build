# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

xdp_pipewire_includes = [include_directories('.')]

subdir('pw-module')
subdir('wp-plugin')

install_data(
  'pipewire.conf',
  rename: '10-xdg-desktop-portal.conf',
  install_dir: pipewire_conf_d_dir,
)

install_data(
  'wireplumber.conf',
  rename: '10-xdg-desktop-portal.conf',
  install_dir: wireplumber_conf_d_dir,
)
