wp_name = 'xdg-desktop-portal'
wp_datadir = datadir / 'wireplumber'

install_data(
  configure_file(
    input: 'wp.conf.in',
    output: wp_name + '.conf',
    configuration: configuration_data({ 'name': wp_name })
  ),
  install_dir: wp_datadir / 'wireplumber.conf.d',
)

install_data(
  'output-nodes.lua',
  install_dir: wp_datadir / 'scripts' / wp_name
)