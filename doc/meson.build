manpages = []

if rst2man.found()
  manpage_conf = configuration_data()
  manpage_conf.set('DATADIR', datadir)
  manpage_conf.set('SYSCONFDIR', sysconfdir)

  rst2man_flags = [
    '--syntax-highlight=none',
  ]

  man_pages = [
    { 'input': 'portals.conf.rst.in', 'output': 'portals.conf', 'section': '5' },
  ]

  foreach man_page: man_pages
    man_input = configure_file(
      input: man_page.get('input'),
      output: man_page.get('input').split('.in').get(0),
      configuration: manpage_conf,
    )
    man_output = man_page.get('output', man_input)
    man_section = man_page.get('section', '1')
    man_full = '@0@.@1@'.format(man_output, man_section)

    manpages += custom_target('man-' + man_output,
      input: man_input,
      output: man_full,
      command: [ rst2man, rst2man_flags, '@INPUT@' ],
      capture: true,
      install: true,
      install_dir: get_option('mandir') / 'man@0@'.format(man_section),
    )
  endforeach
endif

if build_documentation
  # Gather the XML files under data
  all_interfaces_xml = []
  foreach i: portal_sources
    all_interfaces_xml += i
  endforeach
  foreach i: portal_impl_sources
    all_interfaces_xml += i
  endforeach

  # Replace the extension for gdbus-codegen
  all_interfaces_rst = []
  foreach i: all_interfaces_xml
    f = fs.name(i)
    all_interfaces_rst += fs.replace_suffix(f, '.rst')
  endforeach

  interfaces_rst = custom_target('interfaces_rst',
    input: all_interfaces_xml,
    output: all_interfaces_rst,
    command: [ gdbus_codegen, '--generate-rst', 'doc/doc', '@INPUT@' ],
  )

  docs_sources = [
    'conf.py',

    'index.rst',
    'common-conventions.rst',
    'portal-interfaces.rst',
    'implementation-interfaces.rst',
  ]

  copied_docs_sources = []
  foreach d: docs_sources
    copied_docs_sources += configure_file(input: d, output: d, copy: true)
  endforeach

  docs_inputs = copied_docs_sources + interfaces_rst
  if rst2man.found()
    docs_inputs += manpages
  endif

  custom_target('docs',
    input: docs_inputs,
    output: 'html',
    command: [ sphinx_build, meson.current_build_dir(), '@OUTPUT@' ],
    build_by_default: true,
    depend_files: all_interfaces_xml,
  )
endif
