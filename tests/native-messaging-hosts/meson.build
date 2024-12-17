configure_file(input: 'server.sh',
  output: '@PLAINNAME@',
  copy: true,
  install_mode: 'rwxr-xr-x',
  install: enable_installed_tests,
  install_dir: installed_tests_dir / 'native-messaging-hosts',
)

config = configuration_data()
config.set('server_path', meson.current_build_dir() / 'server.sh')
configure_file(input: 'org.example.testing.json.in',
  output: '@BASENAME@',
  configuration: config,
)

# create a second version to be installed
if enable_installed_tests
  config = configuration_data()
  config.set('server_path', installed_tests_dir / 'native-messaging-hosts/server.sh')
    configure_file(input: 'org.example.testing.json.in',
    output: 'installed-org.example.testing.json',
    configuration: config,
  )
  install_data(meson.current_build_dir() / 'installed-org.example.testing.json',
    rename: ['org.example.testing.json'],
    install_dir: installed_tests_dir / 'native-messaging-hosts',
  )
endif
