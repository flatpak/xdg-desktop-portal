permission_store_built_sources = gnome.gdbus_codegen(
  'permission-store-dbus',
  sources: '../data/org.freedesktop.impl.portal.PermissionStore.xml',
  interface_prefix: 'org.freedesktop.impl.portal',
  namespace: 'Xdg',
)

db_sources = files(
  'permission-db.c',
  'gvdb/gvdb-reader.c',
  'gvdb/gvdb-builder.c',
)

xdg_permission_store_sources = [
  'permission-store.c',
  'xdg-permission-store.c',
  xdp_utils_sources,
  db_sources,
  sd_escape_sources,
  permission_store_built_sources,
]

xdg_permission_store = executable(
  'xdg-permission-store',
  xdg_permission_store_sources,
  dependencies: xdg_desktop_portal_deps,
  include_directories: incs_xdg_desktop_portal,
  install: true,
  install_dir: libexecdir,
)

configure_file(
  input: 'xdg-permission-store.service.in',
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: systemd_userunit_dir,
)

document_portal_built_sources = gnome.gdbus_codegen(
  'document-portal-dbus',
  sources: [
      '../data/org.freedesktop.portal.Documents.xml',
      '../data/org.freedesktop.portal.FileTransfer.xml',
  ],
  interface_prefix: 'org.freedesktop.portal',
  namespace: 'XdpDbus',
)

xdg_document_portal_sources = [
  'document-portal.c',
  'file-transfer.c',
  'document-store.c',
  'document-portal-fuse.c',
  xdp_utils_sources,
  db_sources,
  sd_escape_sources,
  document_portal_built_sources,
  permission_store_built_sources,
]

xdg_document_portal = executable(
  'xdg-document-portal',
  xdg_document_portal_sources,
  dependencies: [xdg_desktop_portal_deps, fuse3_dep],
  include_directories: incs_xdg_desktop_portal,
  install: true,
  install_dir: libexecdir,
)

configure_file(
  input: 'xdg-document-portal.service.in',
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: systemd_userunit_dir,
)


doc_portal_service_file_sources = files('org.freedesktop.portal.Documents.service.in')
doc_portal_service_file = configure_file(
  input: doc_portal_service_file_sources,
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: dbus_service_dir,
)

permission_portal_service_file_sources = files('org.freedesktop.impl.portal.PermissionStore.service.in')
permission_portal_service_file = configure_file(
  input: permission_portal_service_file_sources,
  output: '@BASENAME@',
  configuration: base_config,
  install: true,
  install_dir: dbus_service_dir,
)