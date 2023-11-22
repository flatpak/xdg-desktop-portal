.. XDG Desktop Portals documentation master file, created by
   sphinx-quickstart on Thu Aug 24 16:58:13 2023.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

XDG Desktop Portal
==================

XDG Desktop Portal allow `Flatpak apps <http://www.flatpak.org>`_, and other desktop
containment frameworks, to interact with the system in a secure and well defined way.

Content Overview
----------------

The following sections contain relevant information for **app and desktop developers**:

* :doc:`Background apps monitoring <background-app-monitor>`: service that provides information about apps running in background to **desktop developers**.
* :doc:`Common conventions <common-conventions>`: coding patterns and principles common when **app and desktop developers** are working with portal APIs.
* :doc:`Portal interfaces <portal-interfaces>`: portals that **apps** can use to interact with the host system.
* :doc:`Portal backend interfaces <implementation-interfaces>`: interfaces that **desktop developers** can implement.

XDG Desktop Portal backends are selected and can be configured by using one or more
configuration files. :doc:`Read more about them here <portals.conf>`.

.. toctree::
   :maxdepth: 1
   :hidden:

   portals.conf
   common-conventions
   portal-interfaces
   implementation-interfaces
   background-app-monitor
