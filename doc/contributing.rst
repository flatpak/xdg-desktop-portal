Contributing
============

Before developing features or fixing bugs, please make sure you are have done
the following:

- Your code is not on the ``main`` branch of your fork.
- The code has been tested.
- All commit messages are properly formatted and commits squashed where
  appropriate.
- You have included updates to all appropriate documentation.

We use GitHub pull requests to review contributions. Please be kind and patient
as code reviews can be long and minutious.


Development
-----------

XDG Desktop Portal usually runs as a user session service, initialized on
demand through D-Bus activation. It usually starts with the session though,
as many desktop environments try to talk to XDG Desktop Portal on startup.
XDG Desktop Portal initializes specific backends through D-Bus activation
as well.


Building
--------

To build XDG Desktop Portal, first make sure you have the build dependencies
installed through your distribution's package manager. With them installed,
run:

.. code-block:: shell

   meson setup . _build
   meson compile -C _build

Some distributions install portal configuration files in ``/usr``, while Meson
defaults to the prefix ``/usr/local``. If the portal configuration files in your
distribution are in ``/usr/share/xdg-desktop-portal/portals``, re-configure
Meson using ``meson setup --reconfigure . _build --prefix /usr`` and compile
again.


Running
-------

XDG Desktop Portal needs to own the D-Bus name and replace the user session
service that might already be running. To do so, run:

.. code-block:: shell

   _build/src/xdg-desktop-portal --replace

You may need to restart backends after replacing XDG Desktop Portal (please
replace ``[name]`` with the backend name, e.g. ``gnome`` or ``kde`` or ``wlr``):

.. code-block:: shell

   systemctl --user restart xdg-desktop-portal-[name].service

Testing
-------

To execute the test suite present in XDG Desktop Portal, make sure you built it
with ``-Dlibportal=enabled``, and run:

.. code-block:: shell

   meson test -C _build

Documentation
-------------

These instructions are for Fedora, where you will need these packages:

.. code-block::

   sudo dnf install json-glib-devel fuse3-devel gdk-pixbuf2-devel pipewire-devel python3-sphinx flatpak-devel python3-furo python-sphinxext-opengraph python-sphinx-copybutton

Then you can build the website with:

.. code-block:: shell

   meson setup . _build -Ddocumentation=enabled
   ninja -C _build

Then just load the build website into a browser of your choice from
``_build/doc/html/index.html``