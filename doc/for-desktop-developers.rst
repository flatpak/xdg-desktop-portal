For Desktop Developers
======================

The separation of the portal infrastructure into frontend and backend is a clean
way to provide suitable user interfaces that fit into different desktop
environments, while sharing the portal frontend.

The portal backends are focused on providing user interfaces and accessing
session- or host-specific APIs and resources. Details of interacting with the
containment infrastructure such as checking access, registering files in the
Document portal, etc., are handled by the portal frontend.

Portal backends can be layered together. For example, in a GNOME session, most
portal backend interfaces are implemented by the GNOME portal backend, but
the :doc:`org.freedesktop.impl.portal.Access <doc-org.freedesktop.impl.portal.Access>`
interface is implemented by GNOME Shell.


.. toctree::
   :maxdepth: 1

   writing-a-new-backend
   configuration-file

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
