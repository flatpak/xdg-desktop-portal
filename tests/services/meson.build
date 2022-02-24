conf_service = configuration_data()
conf_service.set('libexecdir', meson.project_build_root() / 'document-portal')

configure_file(input: doc_portal_service_file_sources,
               output: '@BASENAME@',
               configuration: conf_service,
               )
configure_file(input: permission_portal_service_file_sources,
               output: '@BASENAME@',
               configuration: conf_service,
               )
