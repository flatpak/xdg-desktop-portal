if build_docbook
  xslfile = files('xmlto-config.xsl')

  portal_docs_xml = configure_file(
    input: 'portal-docs.xml.in',
    output: '@BASENAME@',
    configuration: {
      'VERSION': meson.project_version(),
    },
  )

  docs = custom_target(
    'html',
    build_by_default: true,
    depends: [portal_built_sources, impl_built_sources],
    input: portal_docs_xml,
    output: 'portal-docs.html',
    command: [xmlto, get_option('xmlto-flags'), 'xhtml-nochunks',
              '-o', '@OUTDIR@',
              '-m', xslfile,
              '--searchpath', meson.project_build_root() / 'src',
              '@INPUT@'],
    install: true,
    install_dir: docs_dir,
  )
  doc_extra = files(
      'docbook.css',
      'redirect.html'
  )
  foreach f : doc_extra
      configure_file(
        input: f,
        output: '@PLAINNAME@',
        copy: true,
        install: true,
        install_dir: docs_dir,
      )
  endforeach
endif
