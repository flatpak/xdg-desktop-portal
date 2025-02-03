# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_logger, ImplRequest, ImplSession

import dbus.service
from gi.repository import GLib
from enum import Enum
from dbusmock import MOCK_IFACE


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Inhibit"
VERSION = 3


class SessionState(Enum):
    RUNNING = 1
    QUERY_END = 2
    ENDING = 3


logger = init_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )
    mock.sessions: dict[str, ImplSession] = {}
    mock.session_timers = {}


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossua{sv}",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def Inhibit(self, handle, app_id, window, flags, options, cb_success, cb_error):
    logger.debug(f"Inhibit({handle}, {app_id}, {window}, {flags}, {options})")

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    if self.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(self.response, {}), delay=self.delay)


@dbus.service.method(
    MOCK_IFACE,
    in_signature="s",
    out_signature="",
)
def ArmTimer(self, session_handle):
    self.EmitSignal(
        MAIN_IFACE,
        "StateChanged",
        "oa{sv}",
        [
            session_handle,
            {
                "screensaver-active": False,
                "session-state": SessionState.QUERY_END.value,
            },
        ],
    )

    def close_session():
        session = self.sessions[session_handle]
        session.close()
        self.sessions[session_handle] = None

    if session_handle in self.session_timers:
        GLib.source_remove(self.session_timers[session_handle])
    self.session_timers[session_handle] = GLib.timeout_add(700, close_session)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ooss",
    out_signature="u",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateMonitor(self, handle, session_handle, app_id, window, cb_success, cb_error):
    logger.debug(f"CreateMonitor({handle}, {session_handle}, {app_id}, {window})")

    session = ImplSession(self, BUS_NAME, session_handle, app_id).export()
    self.sessions[session_handle] = session

    # This is irregular: the backend doesn't return the results vardict
    def internal_cb_success(response, results):
        cb_success(response)

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        internal_cb_success,
        cb_error,
    )

    if self.expect_close:
        request.wait_for_close()
    else:

        def arm_timer():
            self.ArmTimer(session_handle)

        request.respond(
            Response(self.response, {}), delay=self.delay, done_cb=arm_timer
        )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="o",
    out_signature="",
)
def QueryEndResponse(self, session_handle):
    try:
        logger.debug(f"QueryEndResponse({session_handle})")

        self.ArmTimer(session_handle)

    except Exception as e:
        logger.critical(e)
