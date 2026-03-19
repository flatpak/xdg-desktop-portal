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

install_data(
  'pipewire.conf',
  rename: '10-xdg-desktop-portal.conf',
  install_dir: pipewire_conf_d_dir,
)