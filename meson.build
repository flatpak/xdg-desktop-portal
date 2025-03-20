project(
  'xdg-desktop-portal',
  'c',
  version: '1.21.0',
  meson_version: '>= 0.60',
  license: 'LGPL-2.0-or-later',
  default_options: [
    'warning_level=2',
  ],
)

###### various directories we'll use later
# foodir are built-in ones, foo_dir are our options

prefix = get_option('prefix')
datadir = prefix / get_option('datadir')
libdir = prefix / get_option('libdir')
libexecdir = prefix / get_option('libexecdir')
sysconfdir = prefix / get_option('sysconfdir')
localedir = prefix / get_option('localedir')
dbus_service_dir = get_option('dbus-service-dir')
if dbus_service_dir == ''
    dbus_service_dir = prefix /  datadir / 'dbus-1' / 'services'
endif

flatpak_intf_dir = get_option('flatpak-interfaces-dir')
if flatpak_intf_dir == ''
    flatpak_dep = dependency('flatpak', version: '>= 1.5.0', required: get_option('flatpak-interfaces'))
    if flatpak_dep.found()
      flatpak_intf_dir = flatpak_dep.get_variable(pkgconfig: 'interfaces_dir')
    endif
endif

systemd_userunit_dir = get_option('systemd-user-unit-dir')
if systemd_userunit_dir == ''
    # This is deliberately not ${libdir}: systemd units always go in
    # .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    systemd_userunit_dir = prefix / 'lib' / 'systemd' / 'user'
endif

dataroot_dir = get_option('datarootdir')
if dataroot_dir == ''
    dataroot_dir = datadir
endif

installed_tests_dir = prefix / libexecdir / 'installed-tests' / meson.project_name()
installed_tests_data_dir = prefix / datadir / 'installed-tests' / meson.project_name()
docs_dir = datadir / 'doc' / meson.project_name()

summary({
	'DBus service dir': dbus_service_dir,
	'Flatpak interfaces dir': flatpak_intf_dir,
	'systemd user unit dir': systemd_userunit_dir,
	'Installed tests dir': installed_tests_dir,
	},
	section: 'Directories',
)

###### various include directories we'll use later
# These are set here so meson handles the relative paths correctly,
# makes life easier for us

common_includes = include_directories('.')  # config.h
src_includes = include_directories('src')

###### plugins, dependencies, compiler setup

i18n = import('i18n')
gnome = import('gnome')
pkgconfig = import('pkgconfig')
pymod = import('python')

cc = meson.get_compiler('c')
cflags = [
    '-Wno-unused-parameter',
    '-Wno-sign-compare',
    '-Wno-missing-field-initializers',
]
add_project_arguments(cc.get_supported_arguments(cflags), language : 'c')

config_h = configuration_data()
config_h.set('_GNU_SOURCE', 1)
config_h.set_quoted('G_LOG_DOMAIN', 'xdg-desktop-portal')
config_h.set_quoted('DATADIR', datadir)
config_h.set_quoted('LIBEXECDIR', libexecdir)
config_h.set_quoted('LOCALEDIR', localedir)
config_h.set_quoted('SYSCONFDIR', sysconfdir)
config_h.set_quoted('GETTEXT_PACKAGE', 'xdg-desktop-portal')
config_h.set_quoted('PACKAGE_STRING', 'xdg-desktop-portal @0@'.format(meson.project_version()))

foreach ident : [
        ['renameat2',         '''#include <stdio.h>'''],
        ['splice',            '''#include <fcntl.h>'''],
        ['pidfd_open',        '''#include <sys/pidfd.h>'''], # since glibc-2.36
]
  have = cc.has_function(ident[0], prefix: ident[1], args: '-D_GNU_SOURCE')
  config_h.set10('HAVE_' + ident[0].to_upper(), have)
endforeach

foreach h : [
  ['sys/vfs.h', 'HAVE_SYS_VFS_H'],
  ['sys/mount.h', 'HAVE_SYS_MOUNT_H'],
  ['sys/statfs.h', 'HAVE_SYS_STATFS_H'],
  ['sys/xattr.h', 'HAVE_SYS_XATTR_H'],
  ['sys/extattr.h', 'HAVE_SYS_EXTATTR_H'],
]
  config_h.set10(h[1], cc.has_header(h[0]))
endforeach

glib_dep = dependency('glib-2.0', version: '>= 2.76')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
json_glib_dep = dependency('json-glib-1.0')
fuse3_dep = dependency('fuse3', version: '>= 3.10.0')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
gst_pbutils_dep = dependency('gstreamer-pbutils-1.0')
geoclue_dep = dependency(
  'libgeoclue-2.0',
  version: '>= 2.5.2',
  required: get_option('geoclue'),
)
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.2.90')
libsystemd_dep = dependency('libsystemd', required: get_option('systemd'))
gudev_dep = dependency('gudev-1.0', required: get_option('gudev'))
umockdev_dep = dependency('umockdev-1.0', required: get_option('tests'))
speechprovider_dep = dependency(
  'speech-provider-1.0',
  version: '>= 1.0.3',
  required: get_option('speech'),
  fallback: ['libspeechprovider', 'speech_provider_lib_dep'],
  default_options: [
    'docs=false',
    'tests=false',
    'introspection=false',
  ],
)

gst_inspect = find_program('gst-inspect-1.0', required: false)
if gst_inspect.found()
  have_wav_parse = run_command(
    gst_inspect, 'wavparse', '--exists',
    check: false,
  ).returncode() == 0
else
  have_wav_parse = false
endif

pytest = find_program('pytest-3', 'pytest', required: get_option('tests'))
python = pymod.find_installation(
  'python3',
  modules: ['dbus', 'dbusmock', 'gi'],
  required: get_option('tests'),
)

bwrap = find_program('bwrap', required: get_option('sandboxed-image-validation').allowed() or get_option('sandboxed-sound-validation').allowed())

if not bwrap.found()
  warning('''
    Sandboxed image and sound validation with Bubblewrap is DISABLED.
    If your system can run Bubblewrap, it's **hightly** recommended that you enable this
    option. Bitmap and sound validation and processing is a common attack vector.
    By proceeding with sandboxed image and sound validation disabled, you acknowledge that you
    are lowering your system's security, and are subject to known or unknown exploits.
    ''')
endif

have_geoclue = geoclue_dep.found()
config_h.set10('HAVE_GEOCLUE', have_geoclue)

have_libsystemd = libsystemd_dep.found()
config_h.set10('HAVE_LIBSYSTEMD', have_libsystemd)

have_gudev = gudev_dep.found()
config_h.set10('HAVE_GUDEV', have_gudev)

have_speech = speechprovider_dep.found()
if have_speech
  config_h.set('HAVE_SPEECH', 1)
endif

add_project_arguments(['-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_72'], language: 'c')

build_documentation = false
gdbus_codegen = find_program('gdbus-codegen', native: true, required: get_option('documentation'))
sphinx_build = find_program('sphinx-build', native: true, required: get_option('documentation'))
sphinx_modules = pymod.find_installation('python3', modules: ['sphinxext.opengraph', 'sphinx_copybutton', 'furo'], required: get_option('documentation'))
if not get_option('documentation').disabled() and gdbus_codegen.found() and sphinx_build.found() and sphinx_modules.found()
    fs = import('fs')
    # we're going to copy this file in to our build tree
    if fs.is_file(flatpak_intf_dir / 'org.freedesktop.portal.Flatpak.xml')
        build_documentation = true
    elif get_option('documentation').enabled()
        error('Flatpak development files are required to build DocBook docs')
    endif
endif

rst2man = find_program('rst2man', 'rst2man.py', required: get_option('man-pages'))

###### systemd units, dbus service files, pkgconfig

base_config = configuration_data()
base_config.set('prefix', prefix)
base_config.set('datadir', datadir)
base_config.set('datarootdir', dataroot_dir)
base_config.set('libexecdir', libexecdir)
base_config.set('VERSION', meson.project_version())
base_config.set('extraargs', '')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
  name: 'xdg-desktop-portal',
  description: 'Desktop integration portal',
  dataonly: true,
  variables: {
    'prefix': get_option('prefix'),
    'datarootdir': dataroot_dir,
    'datadir': '${prefix}/@0@'.format(get_option('datadir')),
    'interfaces_dir': '${datadir}/dbus-1/interfaces/',
  },
)

###### subdirs

subdir('data')
subdir('src')
subdir('document-portal')
subdir('po')
subdir('doc')

enable_tests = get_option('tests') \
  .require(pytest.found()) \
  .require(python.found() and python.language_version().version_compare('>=3.9'),
           error_message: 'Python version >=3.9 is required') \
  .require(umockdev_dep.found()) \
  .require(have_wav_parse,
           error_message: 'gst-inspect and the wavparse plugins are required') \
  .allowed()

enable_installed_tests = get_option('installed-tests')

if enable_tests
  subdir('tests')
endif

###### generate config.h
configure_file(output: 'config.h', configuration: config_h)

summary({
    'Enable documentation': build_documentation,
    'Enable libsystemd support': have_libsystemd,
    'Enable geoclue support': have_geoclue,
    'Enable gudev support': have_gudev,
    'Enable test suite': enable_tests,
    'Enable installed tests:': enable_installed_tests,
    'Build man pages': rst2man.found(),
    'Build flatpak interfaces': flatpak_intf_dir != '',
    'Sandboxed image validation': bwrap.found(),
  },
  section: 'Optional builds',
  bool_yn: true,
)
