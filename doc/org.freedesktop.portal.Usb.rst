..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Device identifiers
^^^^^^^^^^^^^^^^^^

Individual devices are assigned a unique identifier by the portal,
which is used for all further interactions. This unique identifier is
completely random and independent of the device. Permission checks are
in place to not allow apps to try and guess device ids without having
permission to access them.

Permissions
^^^^^^^^^^^

There are two dynamic permissions managed by the USB portal in the
permission store:

1. Blanket USB permission: per-app permission to use any methods of
   the USB portal. Without this permission, apps must not be able to do
   anything — enumerate, monitor, or acquire — with the USB portal.
   [#usb-release]_

2. Specific device permission: per-app permission to acquire a
   specific USB device, down to the serial number.

Enumerating devices
^^^^^^^^^^^^^^^^^^^

There are two ways for apps to learn about devices:

- Apps can call the ``EnumerateDevices()`` method, which gives a snapshot
  of the current devices to the app.

- Apps can create a device monitoring session with ``CreateSession()``
  which sends the list of available devices on creation, and also
  notifies the app about connected and disconnected devices.

Only devices that the app is allowed to see are reported in both
cases.

The udev properties exposed by device enumeration are limited to a
well-known subset of properties.

Device acquisition and release
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Once an app has determined which devices it wants to access, the app
can call the ``AcquireDevices()`` method. This method may prompt a dialog
for the user to allow or deny the app from accessing specific devices.

If permission is granted, xdg-desktop-portal tries to open the device
file on the behalf of the requesting app, and pass the file
descriptor down. [#usb-logind]_

The caller must then call ``FinishAcquireDevices()`` until it indicates it
is finished. It is only necessary to call it more than once if there
are too many file descriptors to return. This is a D-Bus
limitation. Check the ``finished`` return argument.

Using the file descriptor
"""""""""""""""""""""""""

The file descriptors returned by the portal are meant to be used with
the USB library of your choice.

In the case of libusb 1.0.23 and later, use ``libusb_wrap_sys_device()``
and pass the file descriptor as the ``sys_dev`` argument. Note that
libusb must be compiled with udev support (the default) in order to
work.

If you use libhidapi, use the function
``hid_libusb_wrap_sys_device()`` provided by libhidapi-usb.

.. rubric:: Footnotes

.. [#usb-release] Exceptionally, apps can release previously acquired
   devices even when this permission is disabled. This is because there is
   no kernel-side USB revoking yet.

.. [#usb-logind] Opening devices directly is not ideal. The ideal approach
   is to go through logind's ``TakeDevice()`` method, but that adds
   significant complexity since it can only be called by the session
   controller. This can and probably should be implemented in a subsequent
   round of improvements.
