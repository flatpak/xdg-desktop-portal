..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Testing
=======

Unit Tests
----------

Unit tests are written in C using the
`GLib testing framework <https://docs.gtk.org/glib/testing.html>`_.
Files follow the pattern ``tests/test-$NAME.c``.

.. code-block:: bash

   meson test -C build --suite unit

Integration Tests
-----------------

Integration tests test a specific portal in a fully integrated environment.
They are written in Python using pytest and
`dbusmock <https://github.com/martinpitt/python-dbusmock>`_.
Files follow the pattern ``tests/test_$NAME.py``.

.. code-block:: bash

   meson test -C build --suite integration

   # or from the source directory
   ./tests/run-test.sh

The environment is set up by fixtures in ``conftest.py`` which can be overwritten
or parameterized by individual tests. Convenient functions and classes live in
``xdp_utils.py``. Portal backends are implemented using dbusmock templates in the
``tests/templates/`` directory.

Adding New Tests
~~~~~~~~~~~~~~~~

1. Add a ``test_$NAME.py`` file to ``tests/``
2. Add the file to ``tests/meson.build``
3. If the portal requires a backend, add a template to ``tests/templates/``
   and add it to ``tests/meson.build``
4. Make sure the required portals are listed in
   ``xdg_desktop_portal_dir_default_files`` in ``conftest.py``

Look at existing tests for examples of fixtures, request patterns, session
handling, and template parameters.

Running Tests
-------------

.. code-block:: bash

   # All tests
   meson test -C build

   # By suite
   meson test -C build --suite unit
   meson test -C build --suite integration

   # Single test
   meson test -C build integration/clipboard

   # Verbose output
   meson test -C build --verbose

   # Specific pytest test
   ./tests/run-test.sh ./tests/test_clipboard.py::TestClipboard::test_version -v

Environment Variables
---------------------

Required variables are set automatically by meson and ``run-test.sh``:

- ``XDG_DESKTOP_PORTAL_PATH`` -- path to the xdg-desktop-portal binary
- ``XDG_PERMISSION_STORE_PATH`` -- path to the xdg-permission-store binary
- ``XDG_DOCUMENT_PORTAL_PATH`` -- path to the xdg-document-portal binary
- ``XDP_VALIDATE_ICON`` -- path to the icon validator binary
- ``XDP_VALIDATE_SOUND`` -- path to the sound validator binary
- ``XDP_VALIDATE_AUTO`` -- automatically discover validators (for installed
  tests)

Test control:

- ``XDP_TEST_IN_CI`` -- run fewer iterations and test less thoroughly
- ``XDP_TEST_RUN_LONG`` -- run more iterations and enable additional test
  variants (e.g. Snap and Linyaps app info)
- ``XDP_TEST_VALGRIND`` -- signal that Valgrind is running externally so the
  framework increases timeouts
- ``XDP_VALIDATE_ICON_INSECURE`` -- skip bwrap sandboxing for the icon
  validator (for chroot/container environments; never set in production)
- ``XDP_VALIDATE_SOUND_INSECURE`` -- same for the sound validator

Debugging:

- ``G_MESSAGES_DEBUG=all`` -- enable debug output
- ``XDP_DBUS_MONITOR`` -- start dbus-monitor on the test bus
- ``XDP_DBUS_TIMEOUT`` -- D-Bus call timeout in ms (default: 5000)
- ``XDG_DESKTOP_PORTAL_WAIT_FOR_DEBUGGER`` -- pause for debugger attach
  (raises SIGSTOP)
- ``XDG_DOCUMENT_PORTAL_WAIT_FOR_DEBUGGER`` -- same for xdg-document-portal
- ``XDG_PERMISSION_STORE_WAIT_FOR_DEBUGGER`` -- same for xdg-permission-store
