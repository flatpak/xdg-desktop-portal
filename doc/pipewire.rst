..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

PipeWire
========

Some portals relies on giving access to PipeWire nodes to applications.

xdg-desktop-portal provides configuration files to enable its PipeWire module and
WirePlumber plugin.

PipeWire module
---------------

This module performs access control management for clients created by
xdg-desktop-portal.

It connects to the session DBus and subscribes to ``NameOwnerChanged`` signals
of the ``org.freedesktop.portal.Desktop`` name.
The PID of the DBus name owner is xdg-desktop-portal.

A client connection from xdg-desktop-portal PID to PipeWire gets assigned a
``PW_KEY_ACCESS`` set to ``"xdg-desktop-portal"`` and set to permissions ``ALL``.

It is the responsibility of a portal internal to limit the permissions before
passing the connection on to the application. See `PipeWire Access Control
<https://docs.pipewire.org/page_access.html>`_ for details on permissions.

Clients connecting from other PIDs are ignored by this module.

WirePlumber plugin
------------------

This plugin performs access control for clients created by xdg-desktop-portal
at the `session manager  <https://docs.pipewire.org/page_session_manager.html>`_
level.

Camera
""""""

An object manager is in place to watch for nodes that matches the the following
properties:

- ``PW_KEY_MEDIA_ROLE`` is set to ``"Camera"``
- ``PW_KEY_MEDIA_CLASS`` to ``"Video/Source"``

A matching node will be considered as a camera.

Permission Store
""""""""""""""""

An object manager watches for clients with ``PW_KEY_ACCESS`` set to
``"xdg-desktop-portal"`` and ``XDP_PW_KEY_APP_ID`` assigned to a value.

If xdg-desktop-portal assigned ``XDP_PW_KEY_MEDIA_ROLES`` to ``"Camera"``
to the PipeWire client and the application is allowed camera permission, access
to every node detected by the camera object manager will be granted to the
client.

Metadata
""""""""

A metadata object is in place to enable xdp-desktop-portal and WirePlumber to
share information avoiding duplicating code between the daemon and
session-manager.

Besides unrestricted clients, this object can only be accessed by
xdp-desktop-portal clients with:

- ``XDP_PW_KEY_DAEMON`` set to true
- ``XDP_PW_KEY_APP_ID`` unset

Defined metadata:

- ``XDP_PW_KEY_CAMERA_PRESENT``, with subject ``PW_ID_CORE`` and type boolean,
  set to true if the camera object manager find any camera, otherwise false