Writing a New Backend
=====================

Portal backends are session services that provide user interfaces, and access
APIs and resources specific to the desktop environment they are running. For
example, the KDE backend may use KDE-specific technologies, toolkit, and APIs.

For a new portal backend (let's call it ``foo``) to be discoverable by XDG
Desktop Portal, the following steps are necessary:

* Implement one or more :doc:`backend D-Bus interfaces <impl-dbus-interfaces>`
  in an executable. This executable must be `D-Bus activatable
  <https://specifications.freedesktop.org/desktop-entry-spec/1.1/ar01s07.html>`_.
* Install ``foo.portal`` file under ``{DATADIR}/xdg-desktop-portal/portals``.
  Usually, ``{DATADIR}`` is ``/usr/share``. The syntax of the portal file is
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
* ``UseIn``: which desktop environments the portal backend should run. **This
  key is officially deprecated, and has been replaced by the config file**, but
  it's recommended to keep it there for legacy systems.
