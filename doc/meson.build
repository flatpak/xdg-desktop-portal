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

  rst2man = find_program('rst2man', 'rst2man.py', required: false)
  if rst2man.found()
    rst2man_flags = [
      '--syntax-highlight=none',
    ]

    man_pages = [
      { 'input': 'portals-conf.rst', 'output': 'portals.conf', 'section': '5' },
    ]

    foreach man_page: man_pages
      man_input = man_page.get('input')
      man_output = man_page.get('output', man_input)
      man_section = man_page.get('section', '1')
      man_full = '@0@.@1@'.format(man_output, man_section)

      custom_target('man-' + man_output,
        input: man_input,
        output: man_full,
        command: [ rst2man, rst2man_flags, '@INPUT@' ],
        capture: true,
        install: true,
        install_dir: get_option('mandir') / 'man@0@'.format(man_section),
      )
    endforeach
  endif
endif
