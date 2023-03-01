project(
  'xdg-desktop-portal',
  'c',
  version: '1.17.0',
  meson_version: '>= 0.56.2',
  license: 'LGPL-2.0-or-later',
  default_options: ['warning_level=2'])

###### various directories we'll use later
# foodir are built-in ones, foo_dir are our options

prefix = get_option('prefix')
datadir = prefix / get_option('datadir')
libexecdir = prefix / get_option('libexecdir')
localedir = prefix / get_option('localedir')
dbus_service_dir = get_option('dbus-service-dir')
if dbus_service_dir == ''
    dbus_service_dir = prefix /  datadir / 'dbus-1' / 'services'
endif

flatpak_intf_dir = get_option('flatpak-interfaces-dir')
if flatpak_intf_dir == ''
    flatpak_required = host_machine.system() in ['linux']
    flatpak_dep = dependency('flatpak', version: '>= 1.5.0', required : flatpak_required)
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
config_h.set_quoted('GETTEXT_PACKAGE', 'xdg-desktop-portal')
config_h.set_quoted('PACKAGE_STRING', 'xdg-desktop-portal @0@'.format(meson.project_version()))
if cc.has_function('renameat2')
  config_h.set('HAVE_RENAMEAT2', 1)
endif

check_headers = [
  ['sys/vfs.h', 'HAVE_SYS_VFS_H'],
  ['sys/mount.h', 'HAVE_SYS_MOUNT_H'],
  ['sys/statfs.h', 'HAVE_SYS_STATFS_H'],
  ['sys/xattr.h', 'HAVE_SYS_XATTR_H'],
  ['sys/extattr.h', 'HAVE_SYS_EXTATTR_H'],
]

foreach h : check_headers
  config_h.set(h.get(1), cc.has_header(h.get(0)))
endforeach

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
json_glib_dep = dependency('json-glib-1.0')
fuse3_dep = dependency('fuse3', version: '>= 3.10.0')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
geoclue_dep = dependency('libgeoclue-2.0',
                         version: '>= 2.5.2',
                         required: get_option('geoclue'))
libportal_dep = dependency('libportal',
                           required: get_option('libportal'))
pipewire_dep = dependency('libpipewire-0.3',
                          version: '>= 0.2.90',
                          required: get_option('pipewire'))
libsystemd_dep = dependency('libsystemd', required: get_option('systemd'))

bwrap_required = host_machine.system() in ['linux']
bwrap = find_program('bwrap', required: bwrap_required)

if glib_dep.version().version_compare('>= 2.60')
  config_h.set(
    'GLIB_VERSION_MIN_REQUIRED',
    'GLIB_VERSION_2_60',
    description: 'Ignore massive GTimeVal deprecation warnings in 2.62',
  )
endif

if glib_dep.version().version_compare('>= 2.66')
  config_h.set('HAVE_GLIB_2_66', 1)
endif

have_libportal = libportal_dep.found()
if have_libportal
  config_h.set('HAVE_LIBPORTAL', 1)
endif

have_geoclue = geoclue_dep.found()
if have_geoclue
  config_h.set('HAVE_GEOCLUE', 1)
endif

have_pipewire = pipewire_dep.found()
if have_pipewire
  config_h.set('HAVE_PIPEWIRE', 1)
endif

have_libsystemd = libsystemd_dep.found()
if have_libsystemd
  config_h.set('HAVE_LIBSYSTEMD', 1)
endif

build_docbook = false
xmlto = find_program('xmlto', required: get_option('docbook-docs'))
if xmlto.found()
    fs = import('fs')
    # we're going to copy this file in to our build tree
    if fs.is_file(flatpak_intf_dir / 'org.freedesktop.portal.Flatpak.xml')
        build_docbook = true
    elif get_option('docbook-docs').enabled()
        error('Flatpak development files are required to build DocBook docs')
    endif
endif

enable_installed_tests = get_option('installed-tests')

summary({
    'Enable docbook documentation': build_docbook,
    'Enable pipewire support': have_pipewire,
    'Enable libsystemd support': have_libsystemd,
    'Enable geoclue support': have_geoclue,
    'Enable libportal support': have_libportal,
    'Enable installed tests:': enable_installed_tests,
  },
  section: 'Optional builds',
  bool_yn: true,
)

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

subdir('data')
subdir('src')
subdir('document-portal')
subdir('tests')
subdir('po')
subdir('doc')

###### generate config.h
configure_file(output: 'config.h', configuration: config_h)
