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

This plugin performs node access control for clients created by xdg-desktop-portal
at the `session manager  <https://docs.pipewire.org/page_session_manager.html>`_
level.

An object manager watches for clients with ``PW_KEY_ACCESS`` set to
``"xdg-desktop-portal"`` and ``XDP_PW_KEY_APP_ID`` assigned to a value.

Camera
""""""

A node is considered a camera if ``PW_KEY_MEDIA_ROLE`` is set to ``"Camera"``
and ``PW_KEY_MEDIA_CLASS`` to ``"Video/Source"``.

If xdg-desktop-portal assigned ``XDP_PW_KEY_MEDIA_ROLES`` to ``"Camera"``
to the PipeWire client, access to every node considered as camera will be
granted to the client.

An object manager is set to watch node considered as camera, to update allowed
clients if a new matching node is added.

App audio capture
"""""""""""""""""

The plugin owns ``org.freedesktop.impl.portal.AppAudioCapture`` on the session
bus. Each audio stream returned is coming from the playback node of pipewire's
``module-loopback``, providing a single pre-mixed stream from all captured
streams. Once the playback node has a reader, the plugin links to the capture
node:

* for an Application stream, the ``Stream/Output/Audio`` nodes of the requested
  application;
* for a System stream, every ``Stream/Output/Audio`` node.

With the ``exclude_self`` option, we can also exclude the nodes from the
session's application from being captured.

To know which nodes belong to which application, applications are matched by
app id. Currently, ScreenCast backends return the selected window's client pid in
``audio_source``, and xdg-desktop-portal gives that pid to AppAudioCapture when
capturing Application audio, which resolves the app identity from it. On the
other side, we resolve the app identity of PipeWire streams by either getting
direct ``app_id`` props from clients created in sandboxed apps, or by resolving
the app identity via pid.

As future work, this entire process of app identification can be greatly
simplified once appd is supported:

* the backends and compositors can then reliably query and then return app id,
  rather than a pid;
* PipeWire may tag its nodes and clients already with an app id also for host
  applications.
