###### xdg-desktop-portal

portal_built_sources = gnome.gdbus_codegen(
  'xdp-dbus',
  sources: portal_sources + flatpak_intf_sources,
  interface_prefix: 'org.freedesktop.portal',
  namespace: 'XdpDbus',
  docbook: 'portal',
  autocleanup: 'none',
)
impl_built_sources = gnome.gdbus_codegen(
  'xdp-impl-dbus',
  sources: portal_impl_sources,
  interface_prefix: 'org.freedesktop.impl.portal',
  namespace: 'XdpDbusImpl',
  docbook: 'portal',
  autocleanup: 'none',
)
background_monitor_built_sources = gnome.gdbus_codegen(
  'xdp-background-dbus',
  sources: background_monitor_sources,
  interface_prefix: 'org.freedesktop.background',
  namespace: 'XdpDbusBackground',
  docbook: 'portal',
  autocleanup: 'all',
)
if have_geoclue
  geoclue_built_sources = gnome.gdbus_codegen(
    'geoclue-dbus',
    sources: 'org.freedesktop.GeoClue2.Client.xml',
    interface_prefix: 'org.freedesktop.GeoClue2',
    namespace: 'Geoclue',
    autocleanup: 'none',
  )
else
  geoclue_built_sources = []
endif

built_resources = gnome.compile_resources(
  'xdg-desktop-resources',
  'xdg-desktop-portal.gresource.xml',
  c_name: '_xdg_desktop',
  source_dir: ['..'],
)

sd_escape_sources = files('sd-escape.c')

xdp_method_info_built_sources = configure_file(
  command: [find_program('generate-method-info.py'), portal_sources],
  capture: true,
  output: 'xdp-method-info-generated.c',
)

xdp_method_info_sources = files('xdp-method-info.c') + xdp_method_info_built_sources

xdp_utils_includes = include_directories('.')
xdp_utils_sources = files('xdp-utils.c')

if have_libsystemd
  xdp_utils_sources += sd_escape_sources
endif

xdg_desktop_portal_sources = files(
  'account.c',
  'background.c',
  'background-monitor.c',
  'call.c',
  'camera.c',
  'clipboard.c',
  'device.c',
  'documents.c',
  'dynamic-launcher.c',
  'email.c',
  'file-chooser.c',
  'flatpak-instance.c',
  'gamemode.c',
  'glib-backports.c',
  'global-shortcuts.c',
  'inhibit.c',
  'input-capture.c',
  'memory-monitor.c',
  'network-monitor.c',
  'notification.c',
  'open-uri.c',
  'permissions.c',
  'pipewire.c',
  'portal-impl.c',
  'power-profile-monitor.c',
  'print.c',
  'proxy-resolver.c',
  'realtime.c',
  'remote-desktop.c',
  'request.c',
  'restore-token.c',
  'screen-cast.c',
  'screenshot.c',
  'secret.c',
  'session.c',
  'settings.c',
  'trash.c',
  'wallpaper.c',
  'xdg-desktop-portal.c',
)

xdg_desktop_portal_sources += [
  xdp_utils_sources,
  xdp_method_info_sources,
  portal_built_sources,
  impl_built_sources,
  background_monitor_built_sources,
  geoclue_built_sources,
  built_resources,
]

if have_geoclue
  xdg_desktop_portal_sources += files(
    'location.c',
  )
endif

common_deps = [
  glib_dep,
  gio_dep,
  gio_unix_dep,
  json_glib_dep,
]

xdp_utils = shared_library('xdp-utils',
  xdp_utils_sources,
  include_directories: [common_includes],
  dependencies: [common_deps, libsystemd_dep],
  install: false,
)

xdp_utils_enums = gnome.mkenums('xdp-utils-enums',
  sources: ['xdp-utils.h'],
  install_header: false,
)

xdp_utils_gir = gnome.generate_gir(xdp_utils,
  sources: ['xdp-utils.c', 'xdp-utils.h', xdp_utils_enums],
  nsversion: '1.0',
  namespace: 'XdpUtils',
  symbol_prefix: 'xdp',
  identifier_prefix: 'Xdp',
  header: 'xdp-utils.h',
  link_with: xdp_utils,
  includes: ['Gio-2.0'],
  extra_args: ['--accept-unprefixed'],
  install: false,
)

xdg_desktop_portal_deps = common_deps + [
  geoclue_dep,
  pipewire_dep,
  libsystemd_dep,
]

incs_xdg_desktop_portal = [
  include_directories('../document-portal'),
  src_includes,
  common_includes,
]

xdg_desktop_portal = executable(
  'xdg-desktop-portal',
  xdg_desktop_portal_sources,
  dependencies: xdg_desktop_portal_deps,
  include_directories: incs_xdg_desktop_portal,
  install: true,
  install_dir: libexecdir,
)

configure_file(
  input: 'xdg-desktop-portal.service.in',
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: systemd_userunit_dir,
)

configure_file(
  input: 'org.freedesktop.portal.Desktop.service.in',
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: dbus_service_dir,
)

validate_icon_c_args = ['-D_GNU_SOURCE=1']
if bwrap.found()
  validate_icon_c_args += '-DHELPER="@0@"'.format(bwrap.full_path())
endif

executable(
  'xdg-desktop-portal-validate-icon',
  'validate-icon.c',
  dependencies: [gdk_pixbuf_dep],
  c_args: validate_icon_c_args,
  install: true,
  install_dir: libexecdir,
)

configure_file(
  input: 'xdg-desktop-portal-rewrite-launchers.service.in',
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: systemd_userunit_dir,
)

executable(
  'xdg-desktop-portal-rewrite-launchers',
  'rewrite-launchers.c',
  'glib-backports.c',
  dependencies: common_deps,
  include_directories: common_includes,
  install: true,
  install_dir: libexecdir,
)
