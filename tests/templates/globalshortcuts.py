# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_template_logger, ImplRequest, ImplSession
import dbus
import dbus.service
import socket
import time
from dbusmock import MOCK_IFACE, mockobject

from gi.repository import GLib


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.GlobalShortcuts"
VERSION = 1


logger = init_template_logger(__name__)


def load(mock, parameters):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)
    mock.force_close: int = parameters.get("force-close", 0)
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

        session = ImplSession(self, BUS_NAME, session_handle).export()
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
    in_signature="ooa(sa{sv})sa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def BindShortcuts(
    self,
    handle,
    session_handle,
    shortcuts,
    parent_window,
    options,
    cb_success,
    cb_error,
):
    try:
        logger.debug(
            f"BindShortcuts({handle}, {session_handle}, {shortcuts}, {options})"
        )

        assert session_handle in self.sessions
        response = Response(self.response, {})
        request = ImplRequest(self, BUS_NAME, handle)
        request.export()

        def reply():
            logger.debug(f"BindShortcuts with response {response}")
            self.sessions[session_handle].shortcuts = shortcuts
            cb_success(response.response, response.results)

        logger.debug(f"scheduling delay of {self.delay}")
        GLib.timeout_add(self.delay, reply)

    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oo",
    out_signature="ua{sv}",
)
def ListShortcuts(
    self,
    handle,
    session_handle,
):
    shortcuts = self.sessions[session_handle].shortcuts
    return (0, {"shortcuts": shortcuts})


@dbus.service.method(
    MOCK_IFACE,
    in_signature="os",
    out_signature="",
)
def Trigger(self, session_handle, shortcut_id):
    now_since_epoch = int(time.time() * 1000000)
    self.EmitSignal(
        MAIN_IFACE,
        "Activated",
        "osta{sv}",
        [session_handle, shortcut_id, now_since_epoch, {}],
    )
    time.sleep(0.2)
    now_since_epoch = int(time.time() * 1000000)
    self.EmitSignal(
        MAIN_IFACE,
        "Deactivated",
        "osta{sv}",
        [session_handle, shortcut_id, now_since_epoch, {}],
    )
