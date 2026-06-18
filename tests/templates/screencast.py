# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import Response, init_logger, ImplRequest, ImplSession

import dbus
import dbus.service
from dataclasses import dataclass

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.ScreenCast"
VERSION = 6


logger = init_logger(__name__)


@dataclass
class ScreenCastParameters:
    delay: int
    response: int
    expect_close: bool
    version: int
    node_id: int
    pipewire_serial: int


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "screencast_params")
    version = parameters.get("version", VERSION)
    mock.screencast_params = ScreenCastParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        version=version,
        node_id=parameters.get("node-id", 42),
        pipewire_serial=parameters.get("pipewire-serial", 133742),
    )

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(version),
                "AvailableSourceTypes": dbus.UInt32(parameters.get("source-types", 1)),
                "AvailableCursorModes": dbus.UInt32(parameters.get("cursor-modes", 1)),
            }
        ),
    )
    mock.sessions = {}


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateSession(self, handle, session_handle, app_id, options, cb_success, cb_error):
    logger.debug(f"CreateSession({handle}, {session_handle}, {app_id}, {options})")
    params = self.screencast_params

    session = ImplSession(self, BUS_NAME, session_handle, app_id).export()
    self.sessions[session_handle] = session

    request = ImplRequest(self, BUS_NAME, handle, logger, cb_success, cb_error)

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(
            Response(params.response, {"session_handle": session.handle}),
            delay=params.delay,
        )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def SelectSources(self, handle, session_handle, app_id, options, cb_success, cb_error):
    logger.debug(f"SelectSources({handle}, {session_handle}, {app_id}, {options})")
    params = self.screencast_params

    assert session_handle in self.sessions

    request = ImplRequest(self, BUS_NAME, handle, logger, cb_success, cb_error)

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
        f"Start({handle}, {session_handle}, {app_id}, {parent_window}, {options})"
    )
    params = self.screencast_params

    assert session_handle in self.sessions

    request = ImplRequest(self, BUS_NAME, handle, logger, cb_success, cb_error)

    stream_properties = dbus.Dictionary(
        {"size": dbus.Struct((dbus.Int32(1920), dbus.Int32(1080)), signature="ii")},
        signature="sv",
    )
    # The PipeWire serial was added to the stream properties in version 6;
    # an older backend does not advertise it.
    if params.version >= 6:
        stream_properties["pipewire-serial"] = dbus.UInt64(params.pipewire_serial)

    streams = dbus.Array(
        [(dbus.UInt32(params.node_id), stream_properties)],
        signature="(ua{sv})",
    )
    response = Response(params.response, {"streams": streams})

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(response, delay=params.delay)
