..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

For App Developers
==================

XDG Desktop Portal is a session service that provides D-Bus interfaces for apps
to interact with the desktop.

Portal interfaces can be used by sandboxed and unsandboxed apps alike, but
sandboxed apps benefit the most since they don't need special permissions to use
portal APIs. XDG Desktop Portal safeguards many resources and features with a
user-controlled permission system.

The primary goal of portals is to expose common functionality and integration
with the desktop without requiring apps to write desktop-specific code, or
loosen their sandbox restrictions.

.. toctree::
   :hidden:

   reasons-to-use-portals
   convenience-libraries
   api-reference
