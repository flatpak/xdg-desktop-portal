test_portal = configure_file(input: 'test.portal.in',
    output: 'test.portal',
    configuration: test_portal_conf,
    install: enable_installed_tests,
    install_dir: installed_tests_dir / 'portals/test',
)

configure_file(input: 'test-portals.conf',
    output: '@PLAINNAME@',
    copy: true,
    install: enable_installed_tests,
    install_dir: installed_tests_dir / 'portals/test',
)
