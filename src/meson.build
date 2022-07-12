###### xdg-desktop-portal

portal_built_sources = gnome.gdbus_codegen(
  'xdp-dbus',
  sources: portal_sources + flatpak_intf_sources,
  interface_prefix: 'org.freedesktop.portal',
  namespace: 'XdpDbus',
  docbook: 'portal',
  autocleanup: 'objects',
)
impl_built_sources = gnome.gdbus_codegen(
  'xdp-impl-dbus',
  sources: portal_impl_sources,
  interface_prefix: 'org.freedesktop.impl.portal',
  namespace: 'XdpDbusImpl',
  docbook: 'portal',
  autocleanup: 'objects',
)
if have_geoclue
  geoclue_built_sources = gnome.gdbus_codegen(
    'geoclue-dbus',
    sources: 'org.freedesktop.GeoClue2.Client.xml',
    interface_prefix: 'org.freedesktop.GeoClue2',
    namespace: 'Geoclue',
    autocleanup: 'objects',
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

xdg_desktop_portal_sources = files(
  'account.c',
  'background.c',
  'call.c',
  'device.c',
  'documents.c',
  'dynamic-launcher.c',
  'email.c',
  'file-chooser.c',
  'flatpak-instance.c',
  'gamemode.c',
  'glib-backports.c',
  'inhibit.c',
  'memory-monitor.c',
  'network-monitor.c',
  'notification.c',
  'open-uri.c',
  'permissions.c',
  'portal-impl.c',
  'power-profile-monitor.c',
  'print.c',
  'proxy-resolver.c',
  'realtime.c',
  'request.c',
  'screenshot.c',
  'secret.c',
  'session.c',
  'settings.c',
  'trash.c',
  'wallpaper.c',
  'xdg-desktop-portal.c',
  'xdp-utils.c',
)

xdg_desktop_portal_sources += [
  portal_built_sources,
  impl_built_sources,
  geoclue_built_sources,
  built_resources,
]

if have_pipewire
  xdg_desktop_portal_sources += files(
      'screen-cast.c',
      'remote-desktop.c',
      'pipewire.c',
      'camera.c',
  )
endif

if have_geoclue
  xdg_desktop_portal_sources += files(
    'location.c',
  )
endif

if have_libsystemd
  xdg_desktop_portal_sources += sd_escape_sources
endif

common_deps = [
  glib_dep,
  gio_dep,
  gio_unix_dep,
  json_glib_dep,
]

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

xdp_utils_includes = include_directories('.')
xdp_utils_sources = files('xdp-utils.c')

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

executable(
  'xdg-desktop-portal-validate-icon',
  'validate-icon.c',
  dependencies: [gdk_pixbuf_dep],
  c_args: [
      '-D_GNU_SOURCE=1',
      '-DHELPER="@0@"'.format(bwrap.full_path())
  ],
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
