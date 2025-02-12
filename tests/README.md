xdg-desktop-portal test suite
=============================

## Unit tests

This directory contains a number of unit tests. The tests are written in C and
are using the glib testing framework (https://docs.gtk.org/glib/testing.html).

The files follow the pattern `test-$NAME.c` and are compiled by meson. The tests
can be run with `meson test --suite unit`.

## Integration tests

The integration tests usually test a specific portal in a fully integrated
environment. The tests are written in python using the pytest framework.

The files follow the pattern `test_$NAME.py`. The tests can be run with
`meson test --suite integration` or with `run-test.sh` in the source directory.

The environment is being set up by fixtures in `conftest.py` which can be
overwritten or parameterized by the tests themselves. There are a bunch of
convenient functions and classes in `__init__.py`. The portal backends are
implemented using dbusmock templates in the `templates` directory.

### Environment

Some environment variables need to be set for the integration tests to function
properly and the harness will refuse to launch if they are not set. If the
harness is executed by meson or run-test.sh, they will be set automatically.

* `XDG_DESKTOP_PORTAL_PATH`: The path to the xdg-desktop-portal binary

* `XDG_PERMISSION_STORE_PATH`: The path to the xdg-permission-store binary

* `XDG_DOCUMENT_PORTAL_PATH`: The path to the xdg-document-portal binary

* `XDP_VALIDATE_ICON`: The path to the xdg-desktop-portal-validate-icon binary

* `XDP_VALIDATE_SOUND`: The path to the xdg-desktop-portal-validate-sound binary

* `XDP_VALIDATE_AUTO`: If set, automatically discovers the icon and sound
    validators (only useful for installed tests) instead of using
    `XDP_VALIDATE_ICON` and `XDP_VALIDATE_SOUND`.

Some optional environment variables that can be set to influence how the test
harness behaves.

* `XDP_TEST_IN_CI`: If set (to any value), some unreliable tests might get
    skipped and some tests might run less iterations or otherwise test less
    thoroughly.
    Set this for automated QA testing, leave it unset during development.

* `XDP_TEST_RUN_LONG`: If set (to any value), some tests will run more
    iterations or otherwise test more thoroughly

* `FLATPAK_BWRAP`: Path to the **bwrap**(1) executable
    (default: discovered at build-time)

* `XDP_VALIDATE_ICON_INSECURE`: If set (to any value), x-d-p doesn't
    sandbox the icon validator using **bwrap**(1), even if sandboxed
    validation was enabled at compile time.
    This can be used to run build-time tests in a chroot or unprivileged
    container environment, where **bwrap**(1) normally can't work.
    It should never be set on a production system that will be validating
    untrusted icons!

* `XDP_VALIDATE_SOUND_INSECURE`: Same as `XDP_VALIDATE_ICON_INSECURE`,
    but for sounds

Some optional environment variables that can be set to help with debugging.

* `G_MESSAGES_DEBUG=all`: Enable debug output, see
    https://docs.gtk.org/glib/running.html

* `XDP_DBUS_MONITOR`: If set, starts dbus-monitor on the test dbus server

* `XDP_DBUS_TIMEOUT`: Maximum timeout for dbus calls in ms (default: 5s)

* `XDG_DESKTOP_PORTAL_WAIT_FOR_DEBUGGER`: Makes xdg-desktop-portal wait for
    a debugger to attach by raising SIGSTOP

* `XDG_DOCUMENT_PORTAL_WAIT_FOR_DEBUGGER`: Makes xdg-document-portal wait
    for a debugger to attach by raising SIGSTOP

* `XDG_PERMISSION_STORE_WAIT_FOR_DEBUGGER`: Makes xdg-permission-store wait
    for a debugger to attach by raising SIGSTOP

Internal environment variables the tests use via pytest fixtures to set up the
environment they need.

* `XDG_DESKTOP_PORTAL_TEST_APP_ID`: If set, the portal will use a host
    XdpAppInfo with the app id set to the variable. This is used to get a
    predictable app id for tests.

* `XDG_DESKTOP_PORTAL_TEST_USB_QUERIES`: The USB queries for the USB device
    portal testing

* `XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND`: If set, the portal will use a
    XdpAppInfo with the specified kind (`host`, `flatpak`, `snap`). More
    environment variables might be required, depending on the kind.

* `XDG_DESKTOP_PORTAL_TEST_HOST_APPID`: The app id the XdpAppInfo shall be
    using. Must be set if `XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND` is set to
    `host`.

* `XDG_DESKTOP_PORTAL_TEST_FLATPAK_METADATA`: A path to a file containing
    flatpak app metadata the XdpAppInfo shall use. Must be set if
    `XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND` is set to `flatpak`.

* `XDG_DESKTOP_PORTAL_TEST_SNAP_METADATA`: A path to a file containing metadata
    in the format of `snap routine portal-info` the XdpAppInfo shall use. Must
    be set if `XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND` is set to `snap`.

### Adding new tests

Make sure the required portals are listed in
`xdg_desktop_portal_dir_default_files` in `conftest.py`.

Add a `test_${name}.py` file to this directory and add the file to
`meson.build`.

If the portal that is being tested requires a backend implementation, add
it to the `templates` directory and add the file to `meson.build`. See the
dbusmock documentation for details on those templates.
