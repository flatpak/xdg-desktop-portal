# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_logger, ImplRequest, ImplSession
import dbus
import dbus.service
import socket

from gi.repository import GLib


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.RemoteDesktop"
VERSION = 2


logger = init_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)
    mock.force_close: int = parameters.get("force-close", 0)
    mock.force_clipoboard_enabled: bool = parameters.get(
        "force-clipboard-enabled", False
    )
    mock.fail_connect_to_eis: bool = parameters.get("fail-connect-to-eis", False)
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
    try:
        logger.debug(f"CreateSession({handle}, {session_handle}, {app_id}, {options})")

        session = ImplSession(self, BUS_NAME, session_handle, app_id).export()
        self.sessions[session_handle] = session

        response = Response(self.response, {"session_handle": session.handle})

        request = ImplRequest(self, BUS_NAME, handle)

        if self.expect_close:

            def closed_callback():
                response = Response(2, {})
                logger.debug(f"CreateSession Close() response {response}")
                cb_success(response.response, response.results)

            request.export(closed_callback)
        else:
            request.export()

            def reply():
                logger.debug(f"CreateSession with response {response}")
                cb_success(response.response, response.results)

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply)

            if self.force_close > 0:

                def force_close():
                    session.close()

                GLib.timeout_add(self.force_close, force_close)

    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def SelectDevices(self, handle, session_handle, app_id, options, cb_success, cb_error):
    try:
        logger.debug(f"SelectDevices({handle}, {session_handle}, {app_id}, {options})")

        assert session_handle in self.sessions
        response = Response(self.response, {})
        request = ImplRequest(self, BUS_NAME, handle)
        request.export()

        def reply():
            logger.debug(f"SelectDevices with response {response}")
            cb_success(response.response, response.results)

        logger.debug(f"scheduling delay of {self.delay}")
        GLib.timeout_add(self.delay, reply)

    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def Start(
    self, handle, session_handle, app_id, parent_window, options, cb_success, cb_error
):
    try:
        logger.debug(
            f"Start({handle}, {session_handle}, {parent_window}, {app_id}, {options})"
        )

        assert session_handle in self.sessions
        response = Response(self.response, {})

        if self.force_clipoboard_enabled:
            response.results["clipboard_enabled"] = True

        request = ImplRequest(self, BUS_NAME, handle)

        if self.expect_close:

            def closed_callback():
                response = Response(2, {})
                logger.debug(f"Start Close() response {response}")
                cb_success(response.response, response.results)

            request.export(closed_callback)
        else:
            request.export()

            def reply():
                logger.debug(f"Start with response {response}")
                cb_success(response.response, response.results)

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply)

    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="h",
)
def ConnectToEIS(self, session_handle, app_id, options):
    try:
        logger.debug(f"ConnectToEIS({session_handle}, {app_id}, {options})")

        assert session_handle in self.sessions

        if self.fail_connect_to_eis:
            raise dbus.exceptions.DBusException("Purposely failing ConnectToEIS")

        sockets = socket.socketpair()
        self.eis_socket = sockets[0]
        assert self.eis_socket.send(b"HELLO") == 5

        return dbus.types.UnixFd(sockets[1])
    except Exception as e:
        logger.critical(e)
        raise e
