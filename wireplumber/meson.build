xdp_wireplumber_config_h = configuration_data()
if pw_portal_disableable
 xdp_wireplumber_config_h.set('HAVE_PW_XDP', 1)
endif
configure_file(output: 'config.h', configuration: xdp_wireplumber_config_h)

xdp_wireplumber_module_sources = [
  'xdp-wp-permission-manager.c',
  'xdp-wp-plugin.c',
]

xdp_wireplumber_module_sources += impl_built_sources

xdp_wireplumber_module = shared_library(
  'xdg-desktop-portal',
  xdp_wireplumber_module_sources,
  dependencies: [wireplumber_dep, pipewire_dep],
  install: true,
  install_dir: wireplumber_module_dir,
)

install_data(
  'wireplumber.conf',
  rename: '10-xdg-desktop-portal.conf',
  install_dir: wireplumber_conf_d_dir,
)