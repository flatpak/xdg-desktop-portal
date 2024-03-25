env_tests = environment()
env_tests.set('XDP_UNINSTALLED', '1')
env_tests.set('XDG_DATA_DIRS', meson.current_build_dir() / 'share')
env_tests.set('G_TEST_SRCDIR', meson.current_source_dir())
env_tests.set('G_TEST_BUILDDIR', meson.current_build_dir())
env_tests.set('G_DEBUG', 'gc-friendly')  # from glib-tap.mk

if glib_dep.version().version_compare('>= 2.68')
  test_protocol = 'tap'
else
  test_protocol = 'exitcode'
endif

subdir('dbs')
subdir('portals')
subdir('services')
subdir('share')
subdir('native-messaging-hosts')

test_db = executable(
  'testdb',
  ['testdb.c'] + db_sources,
  dependencies: [common_deps],
  include_directories: [common_includes],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'testdb',
  test_db,
  env: env_tests,
  is_parallel: false,
  protocol: test_protocol,
)

if enable_installed_tests
  install_data('dbs/no_tables', install_dir: installed_tests_dir / 'dbs')
endif

test_doc_portal = executable(
  'test-doc-portal',
  'can-use-fuse.c',
  'test-doc-portal.c',
  'utils.c',
  document_portal_built_sources,
  dependencies: [common_deps, fuse3_dep],
  include_directories: [common_includes],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'test-doc-portal',
  test_doc_portal,
  env: env_tests,
  is_parallel: false,
  protocol: test_protocol,
)

test_backends_sources = files(
  'backend/test-backends.c',
  'backend/request.c',
  'backend/session.c',
  'backend/access.c',
  'backend/account.c',
  'backend/appchooser.c',
  'backend/background.c',
  'backend/email.c',
  'backend/filechooser.c',
  'backend/inhibit.c',
  'backend/lockdown.c',
  'backend/notification.c',
  'backend/print.c',
  'backend/screenshot.c',
  'backend/settings.c',
  'backend/wallpaper.c',
  'glib-backports.c',
)

# We build this in the tests/ subdirectory so that it can be accessed
# via G_TEST_BUILT
test_backends = executable(
  'test-backends',
  test_backends_sources,
  document_portal_built_sources,
  document_portal_built_sources,
  impl_built_sources,
  dependencies: [common_deps],
  include_directories: [common_includes, src_includes],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)

test_portals_sources = files(
  'test-portals.c',
  'utils.c',
)

limited_portals_sources = files(
  'limited-portals.c',
  'utils.c',
)

if have_libportal
  extra_portals_sources = files(
    'account.c',
    'background.c',
    'camera.c',
    'email.c',
    'filechooser.c',
    'inhibit.c',
    'location.c',
    'notification.c',
    'openuri.c',
    'print.c',
    'screenshot.c',
    'trash.c',
    'wallpaper.c',
    'web-extensions.c',
    'glib-backports.c',
  )

  test_portals_sources += extra_portals_sources
  limited_portals_sources += extra_portals_sources
endif

test_portals = executable(
  'test-portals',
  impl_built_sources,
  permission_store_built_sources,
  portal_built_sources,
  sd_escape_sources,
  test_portals_sources,
  xdp_utils_sources,
  dependencies: [common_deps, libportal_dep, libsystemd_dep],
  include_directories: [common_includes, xdp_utils_includes],
  c_args: [
    '-DXDG_DP_BUILDDIR="src"',
    '-DXDG_PS_BUILDDIR="document-portal"',
  ],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)

limited_portals = executable(
  'limited-portals',
  impl_built_sources,
  permission_store_built_sources,
  portal_built_sources,
  sd_escape_sources,
  limited_portals_sources,
  xdp_utils_sources,
  dependencies: [common_deps, libportal_dep, libsystemd_dep],
  include_directories: [common_includes, xdp_utils_includes],
  c_args: [
    '-DXDG_DP_BUILDDIR="src"',
    '-DXDG_PS_BUILDDIR="document-portal"',
  ],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)

# Split the portal tests into one test per portal, this makes debugging a lot
# easier.
# Keep in sync with test-portals.c
portal_tests = [
  'account',
  'background',
  'camera',
  'color',
  'email',
  'inhibit',
  'location',
  'notification',
  'openfile',
  'openuri',
  'prepareprint',
  'print',
  'savefile',
  'screenshot',
  'settings',
  'trash',
  'wallpaper',
  'webextensions',
]

test_env = env_tests
test_env.set('XDG_CURRENT_DESKTOP', 'test')
foreach p : portal_tests
    test(
      'test-portals-@0@'.format(p),
      test_portals,
      args: ['--verbose', '--keep-going', '--tap', '-p', '/portal/@0@'.format(p)],
      depends: [test_backends, test_portals],
      env: test_env,
      protocol: test_protocol,
      suite: 'portals',
    )
endforeach

# Split the portal tests into one test per portal, this makes debugging a lot
# easier.
# Keep in sync with test-portals.c
portal_limited = [
  'openfile',
  'savefile',
]

limited_env = env_tests
limited_env.set('XDG_CURRENT_DESKTOP', 'limited')
foreach p : portal_limited
    test(
      'limited-portals-@0@'.format(p),
      limited_portals,
      args: ['--verbose', '--keep-going', '--tap', '-p', '/limited/@0@'.format(p)],
      depends: [test_backends, limited_portals],
      env: limited_env,
      protocol: test_protocol,
      suite: 'portals',
    )
endforeach

if enable_installed_tests
  install_data(
    'session.conf.in',
    'test-document-fuse.sh',
    'test-document-fuse.py',
    install_dir: installed_tests_dir
  )
endif

test_permission_store = executable(
  'test-permission-store',
  'test-permission-store.c',
  'utils.c',
  permission_store_built_sources,
  xdp_utils_sources,
  sd_escape_sources,
  dependencies: [common_deps, libsystemd_dep],
  include_directories: [common_includes, xdp_utils_includes],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'test-permission-store',
  test_permission_store,
  env: env_tests,
  is_parallel: false,
  protocol: test_protocol,
)

test_xdp_utils = executable(
  'test-xdp-utils',
  'test-xdp-utils.c',
  'utils.c',
  xdp_utils_sources,
  sd_escape_sources,
  dependencies: [common_deps, libsystemd_dep],
  include_directories: [common_includes, xdp_utils_includes],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'test-xdp-utils',
  test_xdp_utils,
  env: env_tests,
  is_parallel: false,
  protocol: test_protocol,
)

test_method_info = executable(
  'test-xdp-method-info',
  'test-xdp-method-info.c',
  xdp_method_info_sources,
  dependencies: [common_deps],
  include_directories: [common_includes, xdp_utils_includes],
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'test-xdp-method-info',
  test_method_info,
  env: env_tests,
  is_parallel: true,
  protocol: test_protocol,
)

pytest = find_program('pytest-3', 'pytest', required: get_option('pytest'))
python = pymod.find_installation(
  'python3',
  modules: ['dbus', 'dbusmock', 'gi'],
  required: get_option('pytest'),
)

enable_pytest = pytest.found() and python.found() and python.language_version().version_compare('>=3.9')

if enable_pytest
  subdir('templates')

  pytest_args = ['--verbose', '--log-level=DEBUG']

  # pytest xdist is nice because it significantly speeds up our
  # test process, but it's not required
  if pymod.find_installation('python3', modules: ['xdist'], required: false).found()
    pytest_args += ['-n', 'auto']
  endif

  pytest_files = [
    'conftest.py',
    '__init__.py',
    'test_clipboard.py',
    'test_email.py',
    'test_globalshortcuts.py',
    'test_inputcapture.py',
    'test_remotedesktop.py',
    'test_trash.py',
  ]
  foreach pytest_file : pytest_files
    configure_file(
      input: pytest_file,
      output: pytest_file,
      copy: true,
      install: false
    )

    if pytest_file.startswith('test_')
      testname = pytest_file.replace('.py', '')
      test(
        'pytest @0@'.format(testname),
        pytest,
        args: pytest_args + ['-k', testname],
        suite: ['pytest'],
        timeout: 120,
      )
    endif
  endforeach

  configure_file(
    input: '.pytest.ini',
    output: '@PLAINNAME@',
    copy: true,
    install: false
  )
endif

if enable_installed_tests
  # autotools used to symlink to the host files, here we just install our version
  install_data(
    doc_portal_service_file,
    permission_portal_service_file,
    install_dir: installed_tests_dir / 'services',
  )

  testfiles = [
    'testdb',
    'test-doc-portal',
    'test-document-fuse.sh',
    'test-permission-store',
    'test-xdp-utils',
  ]
  foreach tf : testfiles
      data = configuration_data()
      data.set('installed_testdir', installed_tests_dir)
      data.set('exec', tf)
      configure_file(
        input: 'template.test.in',
        output: '@0@.test'.format(tf),
        configuration: data,
        install: true,
        install_dir: installed_tests_data_dir,
      )
  endforeach

  foreach p : portal_tests
      data = configuration_data()
      data.set('installed_testdir', installed_tests_dir)
      data.set('exec', 'test-portals -p /portal/@0@'.format(p))
      configure_file(
        input: 'template.test.in',
        output: 'test-portals-@0@.test'.format(p),
        configuration: data,
        install: true,
        install_dir: installed_tests_data_dir,
      )
  endforeach

  foreach p : portal_limited
      data = configuration_data()
      data.set('installed_testdir', installed_tests_dir)
      data.set('exec', 'test-portals -p /limited/@0@'.format(p))
      configure_file(
        input: 'template.test.in',
        output: 'test-limited-@0@.test'.format(p),
        configuration: data,
        install: true,
        install_dir: installed_tests_data_dir,
      )
  endforeach
endif

meson.add_devenv(test_env)
