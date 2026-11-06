..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Entitlements
============

Entitlements control what portal functionality a sandboxed app can access. An
entitlement may gate access to an entire portal interface, or to a specific
aspect of a portal. Some entitlements also carry associated data that further
constrains what the app can do. Unsandboxed (host) apps are granted all
entitlements automatically.

How Entitlements Work
---------------------

When a sandboxed app uses a portal, the portal checks whether the app has been
granted the required entitlements. If an entitlement is missing, the call fails
with a ``NotAllowed`` error.

Entitlements can operate at different levels of granularity:

* **Interface-level**: an entitlement gates access to an entire portal interface.
  For example, ``org.freedesktop.portal.Print`` controls access to the Print
  portal.
* **Feature-level**: an entitlement controls a specific capability within a
  portal. For example, ``org.freedesktop.portal.Usb.Devices`` controls which
  USB devices an app can enumerate through the USB portal, and carries data
  specifying the allowed devices.

Entitlements are declared in the app's sandbox metadata. The sandbox runtime
(e.g. Flatpak) is responsible for writing the metadata, and xdg-desktop-portal
reads it to determine the app's entitlements.

Versioning
----------

The entitlement system uses versioning for backwards compatibility. Each
entitlement has a **minimum version** that determines from which entitlement
version onwards the entitlement is enforced.

An app's metadata declares an **entitlement version** that indicates which
version of the entitlement system the app was built against. The versioning
rule is:

* If the app's entitlement version is **lower** than an entitlement's minimum
  version, the entitlement is **implicitly granted**. This ensures that apps
  predating a particular entitlement continue to work without modification.
* If the app's entitlement version is **equal to or higher** than an
  entitlement's minimum version, the entitlement must be **explicitly granted**
  in the app's metadata.

For example, most current entitlements have minimum version 1. An app with
entitlement version 0 (or no entitlement metadata at all) is implicitly granted
all of them. An app that opts in to version 1 must explicitly list every
entitlement it needs.

Flatpak Metadata Format
------------------------

Flatpak apps declare entitlements in the ``[Policy entitlement]`` section of
their metadata. This section is populated using the ``--add-policy`` option of
``flatpak build-finish``, ``flatpak override``, or ``flatpak run``. For
example::

   flatpak build-finish --add-policy=entitlement.grant=org.freedesktop.portal.Print myapp

produces the following metadata:

.. code-block:: ini

   [Policy entitlement]
   grant=org.freedesktop.portal.Print;

Keys
^^^^

``version`` (integer)
   The entitlement version the app is built against. Determines which
   entitlements must be explicitly granted versus implicitly allowed.

``grant`` (string list)
   Semicolon-separated list of entitlement names to grant. Each name is an
   entitlement identifier as documented in the respective portal's API
   reference. Unknown names are ignored, allowing forward compatibility with
   newer portal versions.

Entitlement Data
^^^^^^^^^^^^^^^^

Entitlements that carry additional data use a
``[Policy <entitlement-name>]`` section.

Example
^^^^^^^

.. code-block:: ini

   [Policy entitlement]
   version=1
   grant=org.freedesktop.portal.FileChooser;org.freedesktop.portal.OpenURI;org.freedesktop.portal.Notification;org.freedesktop.portal.Usb;

   [Policy org.freedesktop.portal.Usb.Devices]
   enumerable-devices=1234:5678;abcd:ef01;
   hidden-devices=ffff:0001;

This app declares entitlement version 1 and requests access to the FileChooser,
OpenURI, Notification, and Usb portals. It also declares USB device queries for
specific devices. All other version 1 entitlements will be denied.

.. note::

   For backwards compatibility, ``org.freedesktop.portal.Usb.Devices`` data can
   also be declared in a legacy ``[USB Devices]`` section. Both sections use the
   same keys and are merged if both are present.
