Portal Backends
===============

The separation of the portal infrastructure into frontend and backend is a clean
way to provide suitable user interfaces that fit into different desktop
environments, while sharing the portal frontend.

The portal backends are focused on providing user interfaces and accessing
session- or host-specific APIs and resources. Details of interacting with the
containment infrastructure such as checking access, registering files in the
document portal, etc., are handled by the portal frontend.

Portal backends can be layered together. For example, in a GNOME session, most
portal backend interfaces are implemented by the GNOME portal backend, but
the :doc:`org.freedesktop.impl.portal.Access <doc-org.freedesktop.impl.portal.Access>`
interface is implemented by GNOME Shell.

Adding a New Backend
--------------------

For a new portal backend (let's call it ``foo``) to be discoverable by XDG
Desktop Portal, the following steps are necessary:

* Implement one or more :doc:`backend D-Bus interfaces <impl-dbus-interfaces>`
  in an executable. This executable must be `D-Bus activatable
  <https://specifications.freedesktop.org/desktop-entry-spec/1.1/ar01s07.html>`_.
* Install ``foo.portal`` file under `{DATADIR}/xdg-desktop-portal/portals`.
  Usually, ``{DATADIR}`` is `/usr/share`. The syntax of the portal file is
  documented below.
* Make sure the new backend is picked by XDG Desktop Portal by adding a config
  file that points to the new ``foo`` portal backend. The syntax of the config
  file is documented below.

Portal (`.portal`) File
-----------------------

Portal files are files that allow XDG Desktop Portal to know which portal
backends are available, and which backend D-Bus interfaces they implement.
They usually look like this:

.. code-block::

   [portal]
   DBusName=org.freedesktop.impl.portal.desktop.foo
   Interfaces=org.freedesktop.impl.portal.AppChooser;org.freedesktop.impl.portal.Background;org.freedesktop.impl.portal.Clipboard;org.freedesktop.impl.portal.FileChooser;org.freedesktop.impl.portal.Lockdown;org.freedesktop.impl.portal.RemoteDesktop;org.freedesktop.impl.portal.ScreenCast;
   UseIn=gnome

The following keys are supported in this file:

* ``DBusName``: the D-Bus activation name of the portal backend service.
* ``Interfaces``: which :doc:`backend D-Bus interfaces <impl-dbus-interfaces>`
  this portal backend implements.
* ``UseIn``: which desktop environments the portal backend should run. This key
  is officially deprecated, and has been replaced by the config file (see below),
  but it's recommended to keep it there for legacy systems.

Configuration File
------------------

Desktop systems may have multiple desktop environments and portal backends
installed in parallel, and automatically picking portal backends in this situation
has proven to be a challenge.

For this reason, XDG Desktop Portal uses a config-based matching system. Usually,
the configuration files are provided by the desktop environments themselves, so
that e.g. the GNOME-specific portal backends are picked in GNOME sessions. No
end user intervention is necessary in this case.

Here's an example of a config file distributed by GNOME:

.. code-block::

   [preferred]
   default=gnome;gtk;
   org.freedesktop.impl.portal.Access=gnome-shell;gtk;
   org.freedesktop.impl.portal.Secret=gnome-keyring;

This file specifies that, by default, the ``gnome`` and ``gtk`` portal backends
must be used. However, specifically for :doc:`org.freedesktop.impl.portal.Access
<doc-org.freedesktop.impl.portal.Access>`, the ``gnome-shell`` and ``gtk``
portal backends must be used; and for :doc:`org.freedesktop.impl.portal.Secret
<doc-org.freedesktop.impl.portal.Secret>`, the ``gnome-keyring`` portal
backend must be used.

You can read more about the config file syntax, install locations, and more, in
the portals.conf man page:

.. toctree::
   :maxdepth: 1

   portals.conf

D-Bus Interfaces
----------------

Portal backends must implement one or more backend D-Bus interfaces. The list of
D-Bus interfaces can be found below:

.. toctree::
   :maxdepth: 1

   impl-dbus-interfaces

Background Apps Monitor
-----------------------

In addition to managing the regular interfaces that sandboxed applications
use to interfact with the host system, XDG Desktop Portal also monitors
running applications without an active window - if the portal backend
provides an implementation of the Background portal.

This API can be used by host system services to provide rich interfaces to
manage background running applications.


.. toctree::
   :maxdepth: 1

   doc-org.freedesktop.background.Monitor.rst
