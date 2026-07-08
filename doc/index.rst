..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

.. XDG Desktop Portals documentation master file, created by
   sphinx-quickstart on Thu Aug 24 16:58:13 2023.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

.. image:: _static/xdg-portal-light.png
   :class: only-light
.. image:: _static/xdg-portal-dark.png
   :class: only-dark

XDG Desktop Portal
==================

Portals allow `Flatpak <https://flatpak.org>`__ apps, and other desktop containment
frameworks, to interact with the system in a secure and well defined way.

XDG Desktop Portal works by exposing a series of D-Bus interfaces known as
*portals* to apps. Portals are designed to be usable by all apps, including
ones which are confined by sandboxes, such as Flatpak.

The portal interfaces include APIs for file access, opening
URIs, printing and others.

XDG Desktop Portal works together with desktop environment specific backends to
mediate access to resources and functionality in an integrated manner.

Documentation
-------------

This documentation covers everything you need to know to build apps that use
portals, write portal backends for your desktop environment, configure and
distribute portals as part of a distribution, as well as basic concepts and
common conventions:

* :doc:`Common conventions <common-conventions>`: coding patterns and principles
  common when **app and desktop developers** are working with portal APIs.
* :doc:`App Development <for-app-developers>`: portal APIs that **apps** can use
  to interact with the host system.
* :doc:`Desktop Integration <for-desktop-developers>`: interfaces and
  configuration files that **desktop developers** can implement and install in
  order to write a portal backend.
* :doc:`Contributing <for-contributors>`: how **contributors** can contribute to
  the project.

.. toctree::
   :maxdepth: 2
   :hidden:

   common-conventions
   for-app-developers
   for-desktop-developers
   for-contributors

Using Portals
-------------

Many toolkits and frameworks already integrate with some portals, and you might
already be using them without realizing it. For all other cases, there are
:doc:`Convenience Libraries <convenience-libraries>`
and direct :doc:`D-Bus access <api-reference>`.

Backends
--------

To implement most portals, xdg-desktop-portal relies on a desktop environment
specific backend to provide a part of the functionality.

Here are some examples of available backends:

- GTK `xdg-desktop-portal-gtk <http://github.com/flatpak/xdg-desktop-portal-gtk>`__
- GNOME `xdg-desktop-portal-gnome <https://gitlab.gnome.org/GNOME/xdg-desktop-portal-gnome/>`__
- KDE `xdg-desktop-portal-kde <https://invent.kde.org/plasma/xdg-desktop-portal-kde>`__
- LXQt `xdg-desktop-portal-lxqt <https://github.com/lxqt/xdg-desktop-portal-lxqt>`__
- Pantheon (elementary OS) `xdg-desktop-portal-pantheon <https://github.com/elementary/portals>`__
- wlroots `xdg-desktop-portal-wlr <https://github.com/emersion/xdg-desktop-portal-wlr>`__
- Deepin `xdg-desktop-portal-dde <https://github.com/linuxdeepin/xdg-desktop-portal-dde>`__
- Xapp (Cinnamon, MATE, Xfce) `xdg-desktop-portal-xapp <https://github.com/linuxmint/xdg-desktop-portal-xapp>`__
- COSMIC `xdg-desktop-portal-cosmic <https://github.com/pop-os/xdg-desktop-portal-cosmic>`__

Contributing
------------

XDG Desktop Portal is `Free Software <https://www.gnu.org/philosophy/free-sw.html>`__.
Contributions :doc:`welcome <for-contributors>`.

Are you an app developer and want a new portal or feature? Contribute to or open
a new `discussion <https://github.com/flatpak/xdg-desktop-portal/discussions>`__.

Is there an issue with the portal frontend? Report it to our
`issue tracker <https://github.com/flatpak/xdg-desktop-portal/issues>`__.

Just want to talk or need help? Join our
`matrix <https://matrix.to/#/#xdg-desktop-portals:matrix.org>`__.