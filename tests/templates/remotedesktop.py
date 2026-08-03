# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import socket
from dataclasses import dataclass

import dbus
import dbus.service
from dbusmock import MOCK_IFACE
from gi.repository import GLib

from tests.templates.xdp_utils import ImplRequest, ImplSession, Response, init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.RemoteDesktop"
VERSION = 2


logger = init_logger(__name__)


@dataclass
class RemotedesktopParameters:
    delay: int
    response: int
    expect_close: bool
    force_close: int
    force_clipboard_enabled: bool
    fail_connect_to_eis: bool
    devices: int
    streams: bool
    node_id: int
    stream_size: tuple[int, int]


def load(mock, parameters=None):
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "remotedesktop_params")
    mock.remotedesktop_params = RemotedesktopParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        force_close=parameters.get("force-close", 0),
        force_clipboard_enabled=parameters.get("force-clipboard-enabled", False),
        fail_connect_to_eis=parameters.get("fail-connect-to-eis", False),
        devices=parameters.get("devices", 0),
        streams=parameters.get("streams", False),
        node_id=parameters.get("node-id", 42),
        stream_size=parameters.get("stream-size", (1920, 1080)),
    )

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )
    mock.sessions: dict[str, ImplSession] = {}


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateSession(self, handle, session_handle, app_id, options, cb_success, cb_error):
    logger.debug(f"CreateSession({handle}, {session_handle}, {app_id}, {options})")
    params = self.remotedesktop_params

    session = ImplSession(self, BUS_NAME, session_handle, app_id).export()
    self.sessions[session_handle] = session

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(
            Response(params.response, {"session_handle": session.handle}),
            delay=params.delay,
        )
        if params.force_close > 0:
            GLib.timeout_add(params.force_close, session.close)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def SelectDevices(self, handle, session_handle, app_id, options, cb_success, cb_error):
    logger.debug(f"SelectDevices({handle}, {session_handle}, {app_id}, {options})")
    params = self.remotedesktop_params

    assert session_handle in self.sessions

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(params.response, {}), delay=params.delay)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def Start(
    self, handle, session_handle, app_id, parent_window, options, cb_success, cb_error
):
    logger.debug(
        f"Start({handle}, {session_handle}, {parent_window}, {app_id}, {options})"
    )
    params = self.remotedesktop_params

    assert session_handle in self.sessions

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    response = Response(params.response, {})
    if params.force_clipboard_enabled:
        response.results["clipboard_enabled"] = True

    if params.devices:
        response.results["devices"] = dbus.UInt32(params.devices)

    if params.streams:
        width, height = params.stream_size
        stream_properties = dbus.Dictionary(
            {
                "size": dbus.Struct(
                    (dbus.Int32(width), dbus.Int32(height)), signature="ii"
                )
            },
            signature="sv",
        )

        response.results["streams"] = dbus.Array(
            [(dbus.UInt32(params.node_id), stream_properties)],
            signature="(ua{sv})",
        )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(response, delay=params.delay)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="h",
)
def ConnectToEIS(self, session_handle, app_id, options):
    try:
        logger.debug(f"ConnectToEIS({session_handle}, {app_id}, {options})")
        params = self.remotedesktop_params

        assert session_handle in self.sessions

        if params.fail_connect_to_eis:
            raise dbus.exceptions.DBusException("Purposely failing ConnectToEIS")

        sockets = socket.socketpair()
        self.eis_socket = sockets[0]
        assert self.eis_socket.send(b"HELLO") == 5

        return dbus.types.UnixFd(sockets[1])
    except dbus.exceptions.DBusException as e:
        logger.critical(e)
        raise


@dbus.service.method(MOCK_IFACE, in_signature="s", out_signature="s")
def GetSessionAppId(self, session_handle):
    logger.debug(f"GetSessionAppId({session_handle})")

    assert session_handle in self.sessions
    return self.sessions[session_handle].app_id
