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
  foreach i: background_monitor_sources
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

  fix_rst_dbus = find_program('fix-rst-dbus.py')
  fixed_rst_files = custom_target('fix_rst_dbus',
    input: interfaces_rst,
    output: 'rst-dbus.fixed',
    build_always_stale: true,
    build_by_default: true,
    command: [ fix_rst_dbus, meson.current_build_dir(), 'doc', '@INPUT@' ],
    depends: interfaces_rst,
    depend_files: all_interfaces_xml,
  )

  docs_sources = [
    'conf.py',
    'favicon.ico',

    'api-reference.rst',
    'backends.rst',
    'common-conventions.rst',
    'contributing.rst',
    'impl-dbus-interfaces.rst',
    'index.rst',
    'introduction.rst',
    'requests.rst',
    'sessions.rst',
    'window-identifiers.rst',
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
    depends: fixed_rst_files,
    depend_files: all_interfaces_xml,
  )

  # Copy static files to the appropriate output subdir
  copy = find_program('copy-subdir.py')

  static_files = [
    'bullet.svg',
    'card.png',
    'inter.woff2',
    'toc_bullet.svg',
    'xdg.css',
    'xdg-portal-dark.png',
    'xdg-portal-light.png',
  ]

  foreach f: static_files
    run_command(copy, '_static' / f, 'html' / '_static' / f)
  endforeach
endif
