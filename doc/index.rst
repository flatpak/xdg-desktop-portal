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

XDG Desktop Portal allow `Flatpak apps <http://www.flatpak.org>`_, and other desktop
containment frameworks, to interact with the system in a secure and well defined way.

This documentation covers everything you need to know to build apps that use portals,
write portal backends for your desktop environment, configure and distribute portals
as part of a distribution, as well as basic concepts and common conventions.

The documentation pages target primarily app developers, desktop developers, and
system distributors and administrators. The contents may also be relevant to those
who have a general interest in portals.

Content Overview
----------------

This documentation is made up of the following sections:

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
