###### xdg-desktop-portal

portal_built_sources = gnome.gdbus_codegen(
  'xdp-dbus',
  sources: portal_sources + flatpak_intf_sources,
  interface_prefix: 'org.freedesktop.portal',
  namespace: 'XdpDbus',
  docbook: 'portal',
)
host_built_sources = gnome.gdbus_codegen(
  'xdp-host-dbus',
  sources: portal_host_sources,
  interface_prefix: 'org.freedesktop.host.portal',
  namespace: 'XdpDbusHost',
  docbook: 'portal',
)
impl_built_sources = gnome.gdbus_codegen(
  'xdp-impl-dbus',
  sources: portal_impl_sources,
  interface_prefix: 'org.freedesktop.impl.portal',
  namespace: 'XdpDbusImpl',
  docbook: 'portal',
)
background_monitor_built_sources = gnome.gdbus_codegen(
  'xdp-background-dbus',
  sources: background_monitor_sources,
  interface_prefix: 'org.freedesktop.background',
  namespace: 'XdpDbusBackground',
  docbook: 'portal',
)
if have_geoclue
  geoclue_built_sources = gnome.gdbus_codegen(
    'geoclue-dbus',
    sources: 'org.freedesktop.GeoClue2.Client.xml',
    interface_prefix: 'org.freedesktop.GeoClue2',
    namespace: 'Geoclue',
  )
else
  geoclue_built_sources = []
endif
if have_speech
  speech_built_sources = gnome.gdbus_codegen(
    'speech-provider-dbus',
    sources: speechprovider_dep.get_variable('iface_xml'),
    interface_prefix: 'org.freedesktop.Speech.',
    namespace: 'Speech',
    annotations : [
      ['org.freedesktop.Speech.Provider', 'org.gtk.GDBus.C.Name', 'ProviderProxy']
    ],
    extra_args: '--glib-min-required=2.64',
  )
else
  speech_built_sources = []
endif

enums_sources = gnome.mkenums_simple(
  'xdp-enum-types',
  sources: files(
    'xdp-app-info-private.h',
  ),
)

built_resources = enums_sources + gnome.compile_resources(
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

xdp_utils_deps = []
xdp_utils_includes = include_directories('.')
xdp_utils_sources = enums_sources + files(
  'xdp-app-info.c',
  'xdp-app-info-flatpak.c',
  'xdp-app-info-host.c',
  'xdp-app-info-registry.c',
  'xdp-app-info-snap.c',
  'xdp-app-info-linyaps.c',
  'xdp-sealed-fd.c',
  'xdp-usb-query.c',
  'xdp-utils.c',
)

if have_libsystemd
  xdp_utils_sources += sd_escape_sources
  xdp_utils_deps += [libsystemd_dep]
endif

xdg_desktop_portal_sources = files(
  'account.c',
  'background.c',
  'camera.c',
  'clipboard.c',
  'dynamic-launcher.c',
  'email.c',
  'file-chooser.c',
  'flatpak-instance.c',
  'gamemode.c',
  'global-shortcuts.c',
  'inhibit.c',
  'input-capture.c',
  'memory-monitor.c',
  'network-monitor.c',
  'notification.c',
  'open-uri.c',
  'pipewire.c',
  'power-profile-monitor.c',
  'print.c',
  'proxy-resolver.c',
  'realtime.c',
  'registry.c',
  'remote-desktop.c',
  'screen-cast.c',
  'screenshot.c',
  'secret.c',
  'settings.c',
  'trash.c',
  'wallpaper.c',
  'xdp-app-launch-context.c',
  'xdp-background-monitor.c',
  'xdp-context.c',
  'xdp-documents.c',
  'xdp-main.c',
  'xdp-permissions.c',
  'xdp-portal-config.c',
  'xdp-request.c',
  'xdp-session.c',
  'xdp-session-persistence.c',
)

xdg_desktop_portal_sources += [
  xdp_utils_sources,
  xdp_method_info_sources,
  portal_built_sources,
  host_built_sources,
  impl_built_sources,
  background_monitor_built_sources,
  geoclue_built_sources,
  speech_built_sources,
  built_resources,
]

if have_geoclue
  xdg_desktop_portal_sources += files(
    'location.c',
  )
endif

if have_gudev
  xdg_desktop_portal_sources += files(
    'usb.c',
  )
endif

if have_speech
  xdg_desktop_portal_sources += files(
    'speech.c',
  )
endif

common_deps = [
  glib_dep,
  gio_dep,
  gio_unix_dep,
  json_glib_dep,
]

if gudev_dep.found()
  common_deps += gudev_dep
endif

xdg_desktop_portal_deps = common_deps + [
  geoclue_dep,
  pipewire_dep,
  xdp_utils_deps,
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

xdp_validate_icon = executable(
  'xdg-desktop-portal-validate-icon',
  'validate-icon.c',
  dependencies: [gdk_pixbuf_dep, gio_unix_dep],
  c_args: validate_icon_c_args,
  install: true,
  install_dir: libexecdir,
)

validate_sound_c_args = ['-D_GNU_SOURCE=1']
if bwrap.found()
  validate_sound_c_args += '-DHELPER="@0@"'.format(bwrap.full_path())
endif

xdp_validate_sound = executable(
  'xdg-desktop-portal-validate-sound',
  'validate-sound.c',
  dependencies: [gst_pbutils_dep],
  c_args: validate_sound_c_args,
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
  dependencies: common_deps,
  include_directories: common_includes,
  install: true,
  install_dir: libexecdir,
)
