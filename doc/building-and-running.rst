..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

.. _building-and-running:

Building & Running
==================

Normally, XDG Desktop Portal runs as a user session service, initialized on
demand through D-Bus activation. It usually starts with the session though,
as many desktop environments try to talk to XDG Desktop Portal on startup.
XDG Desktop Portal initializes specific backends through D-Bus activation
as well.

Setup
-----

Run:

.. code-block:: shell

   meson setup --reconfigure . _build

Some distributions install portal configuration files in ``/usr``, while Meson
defaults to the prefix ``/usr/local``. If the portal configuration files in your
distribution are in ``/usr/share/xdg-desktop-portal/portals``, re-configure
Meson using ``meson setup --reconfigure . _build --prefix /usr``.


Building
--------

Host
^^^^

To build XDG Desktop Portal, first make sure you have the build dependencies
installed through your distribution's package manager. With them installed,
run:

.. code-block:: shell

   meson compile -C _build

``systemd-sysext``
^^^^^^^^^^^^^^^^^^

If you are using ``systemd-sysext`` provided by ``sysext-utils``:

.. code-block:: shell

   sysext-build zz-xdg-desktop-portal _build/
   sysext-add zz-xdg-desktop-portal.sysext.raw


Running
-------

XDG Desktop Portal needs to own the D-Bus name and replace the user session
service that might already be running.

Host
^^^^

Run:

.. code-block:: shell

   _build/desktop-portal/xdg-desktop-portal --replace

``systemd-sysext``
^^^^^^^^^^^^^^^^^^

If you are using ``systemd-sysext`` from ``sysext-utils`` to
temporarily replace the service:

.. code-block:: shell

   [prefix]/libexec/xdg-desktop-portal --replace

------------

You may need to restart backends after replacing XDG Desktop Portal (please
replace ``[name]`` with the backend name, e.g. ``gnome`` or ``kde`` or ``wlr``):

.. code-block:: shell

   systemctl --user restart xdg-desktop-portal-[name].service

Testing
-------

To execute the test suite present in XDG Desktop Portal, run:

.. code-block:: shell

   meson test -C _build

Enable the pre-commit hooks to catch issues early:

.. code-block:: shell

   git config --local core.hooksPath githooks/

When a branch is pushed to github, CI will also run tests in a few more
configurations.

Documentation
-------------

These instructions are for Fedora, where you will need these packages:

.. code-block::

   sudo dnf install json-glib-devel fuse3-devel gdk-pixbuf2-devel pipewire-devel python3-sphinx flatpak-devel python3-furo python-sphinxext-opengraph python-sphinx-copybutton

Then you can build and serve the website with:

.. code-block:: shell

   meson setup . _build -Ddocumentation=enabled
   ninja -C _build
   python3 -m http.server 8000 -b 127.0.0.1 -d ./_build/doc/html/

Then just open `http://127.0.0.1:8000/` on a browser of your choice.