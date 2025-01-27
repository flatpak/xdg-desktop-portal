# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_logger, ImplRequest, ImplSession

from dbusmock import MOCK_IFACE
import dbus
import dbus.service
import socket
from gi.repository import GLib
from dataclasses import dataclass


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
    force_clipoboard_enabled: bool
    fail_connect_to_eis: bool


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "remotedesktop_params")
    mock.remotedesktop_params = RemotedesktopParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        force_close=parameters.get("force-close", 0),
        force_clipoboard_enabled=parameters.get("force-clipboard-enabled", False),
        fail_connect_to_eis=parameters.get("fail-connect-to-eis", False),
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
    if params.force_clipoboard_enabled:
        response.results["clipboard_enabled"] = True

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
    except Exception as e:
        logger.critical(e)
        raise e


@dbus.service.method(MOCK_IFACE, in_signature="s", out_signature="s")
def GetSessionAppId(self, session_handle):
    logger.debug(f"GetSessionAppId({session_handle})")

    assert session_handle in self.sessions
    return self.sessions[session_handle].app_id
