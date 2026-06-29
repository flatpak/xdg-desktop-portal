..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

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
   :hidden:

   writing-a-new-backend
   configuration-file
   system-integration
   impl-dbus-interfaces
   doc-org.freedesktop.background.Monitor.rst