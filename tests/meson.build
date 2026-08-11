# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

env_tests = environment()
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
test_permission_db = executable(
  'test-permission-db',
  ['test-permission-db.c'] + db_sources,
  dependencies: [common_deps] + db_deps,
  include_directories: incs_xdg_desktop_portal,
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'unit/permission-db',
  test_permission_db,
  suite: ['unit'],
  env: env_tests,
  is_parallel: false,
  protocol: test_protocol,
)

test_xdp_utils = executable(
  'test-xdp-utils',
  'test-xdp-utils.c',
  xdp_utils_sources,
  dependencies: [common_deps, xdp_utils_deps],
  include_directories: incs_xdg_desktop_portal,
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'unit/xdp-utils',
  test_xdp_utils,
  suite: ['unit'],
  env: env_tests,
  is_parallel: false,
  protocol: test_protocol,
)

test_method_info = executable(
  'test-xdp-method-info',
  'test-xdp-method-info.c',
  xdp_method_info_sources,
  dependencies: [common_deps],
  include_directories: incs_xdg_desktop_portal,
  install: enable_installed_tests,
  install_dir: installed_tests_dir,
)
test(
  'unit/xdp-method-info',
  test_method_info,
  suite: ['unit'],
  env: env_tests,
  is_parallel: true,
  protocol: test_protocol,
)

run_test = find_program('run-test.sh')

pytest_args = ['--verbose', '--log-level=DEBUG']

pytest_env = environment()
pytest_env.set('BUILDDIR', meson.project_build_root())
pytest_env.set('UNDER_MESON', '1')

# pytest xdist is nice because it significantly speeds up our
# test process, but it's not required
if pymod.find_installation('python3', modules: ['xdist'], required: false).found()
  # using auto can easily start too many tests which will block each other
  # a value of around 5 seems to work well
  pytest_args += ['-n', '5']
endif

# Portals that are only built when their dependency is available cannot be
# tested when it is missing: the interface is not on the bus at all.
tests = {
  'account': {},
  'background': {},
  'camera': {},
  'clipboard': {},
  'documents': {},
  'document_fuse': {},
  'dynamiclauncher': {},
  'email': {},
  'filechooser': {},
  'globalshortcuts': {},
  'inhibit': {},
  'inputcapture': {},
  'location': {'enabled': have_geoclue},
  'notification': {'timeout': 225},
  'openuri': {},
  'permission_store': {},
  'print': {},
  'registry': {},
  'remotedesktop': {},
  'settings': {},
  'screencast': {},
  'screenshot': {},
  'shutdown': {},
  'trash': {},
  'usb': {'enabled': have_gudev},
  'wallpaper': {},
}

template_files = [
  'templates/access.py',
  'templates/account.py',
  'templates/appchooser.py',
  'templates/background.py',
  'templates/clipboard.py',
  'templates/dynamiclauncher.py',
  'templates/email.py',
  'templates/filechooser.py',
  'templates/geoclue2.py',
  'templates/globalshortcuts.py',
  'templates/inhibit.py',
  'templates/inputcapture.py',
  'templates/lockdown.py',
  'templates/notification.py',
  'templates/print.py',
  'templates/remotedesktop.py',
  'templates/screencast.py',
  'templates/screenshot.py',
  'templates/settings.py',
  'templates/usb.py',
  'templates/wallpaper.py',
  'templates/xdp_utils.py',
]

pytest_files = []

foreach testname, params : tests
  if not params.get('enabled', true)
    continue
  endif

  pytest_files += 'test_@0@.py'.format(testname)

  test(
    'integration/@0@'.format(testname),
    run_test,
    args: [meson.current_source_dir() / 'test_@0@.py'.format(testname)] + pytest_args,
    depends: [
      xdg_desktop_portal,
      xdp_validate_icon,
      xdp_validate_sound,
      xdg_permission_store,
      xdg_document_portal,
    ],
    env: pytest_env,
    suite: ['integration'],
    timeout: params.get('timeout', 200),
  )
endforeach

if enable_installed_tests
  install_data(
    pytest_files,
    '__init__.py',
    'conftest.py',
    'xdp_utils.py',
    'xdp_doc_utils.py',
    'asan.suppression',
    'valgrind.suppression',
    install_dir: installed_tests_dir / 'tests',
  )
  install_data(
    template_files,
    install_dir: installed_tests_dir / 'tests' / 'templates',
  )

  installed_env = {
    'XDG_DESKTOP_PORTAL_PATH': libexecdir / 'xdg-desktop-portal',
    'XDG_PERMISSION_STORE_PATH': libexecdir / 'xdg-permission-store',
    'XDG_DOCUMENT_PORTAL_PATH': libexecdir / 'xdg-document-portal',
    'XDP_VALIDATE_AUTO': '1',
  }
  env = ''
  foreach key, value : installed_env
    env += f'@key@=@value@ '
  endforeach

  foreach pytest_file : pytest_files
    testname = pytest_file.replace('.py', '').replace('test_', '')

    exec = [pytest.full_path(), installed_tests_dir / 'tests' / pytest_file]
    exec += pytest_args
    exec += ['-p', 'no:cacheprovider']
    exec = ' '.join(exec)

    data = configuration_data()
    data.set('exec', exec)
    data.set('env', env)
    data.set('libdir', libdir)

    configure_file(
      input: 'template.test.in',
      output: 'integration-@0@.test'.format(testname),
      configuration: data,
      install: true,
      install_dir: installed_tests_data_dir,
    )
  endforeach
endif
