# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import os
from dataclasses import dataclass

import dbus
import dbus.service

from tests.templates.xdp_utils import (
    ImplRequest,
    ImplSession,
    Response,
    init_logger,
)

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.ScreenCast"
VERSION = 6


logger = init_logger(__name__)


def pick_audio_type(window: bool, audio_types: int) -> int:
    if window and audio_types & 4:
        return 4
    return audio_types & 2


def audio_source(audio_type: int, app_id: str = "org.example.App") -> dbus.Dictionary:
    source: dict[str, object] = {"audio_type": dbus.UInt32(audio_type)}
    if audio_type == 4:
        source["app_id"] = app_id
        source["pid"] = dbus.UInt32(os.getpid())
    return dbus.Dictionary(source, signature="sv")


@dataclass
class ScreenCastParameters:
    delay: int
    response: int
    expect_close: bool
    version: int
    node_id: int
    pipewire_serial: int
    n_streams: int


def load(mock, parameters=None):
    parameters = parameters or {}

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
        n_streams=parameters.get("n-streams", 1),
    )

    properties = {
        "version": dbus.UInt32(version),
        "AvailableSourceTypes": dbus.UInt32(parameters.get("source-types", 1)),
        "AvailableCursorModes": dbus.UInt32(parameters.get("cursor-modes", 1)),
    }
    if version >= 7:
        # 2: System, 4: Application
        properties["AvailableAudioTypes"] = dbus.UInt32(
            parameters.get("audio-types", 2 | 4)
        )

    mock.AddProperties(MAIN_IFACE, dbus.Dictionary(properties))
    mock.sessions = {}
    mock.source_types = {}
    mock.audio_types = {}
    # An audio source of this type, whatever was asked for
    mock.forced_audio_type = parameters.get("audio-type", 0)


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
    self.source_types[session_handle] = options.get("types", 1)
    self.audio_types[session_handle] = options.get("audio_types", 1)

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

    streams = dbus.Array([], signature="(ua{sv})")
    for i in range(params.n_streams):
        stream_properties = dbus.Dictionary(
            {"size": dbus.Struct((dbus.Int32(1920), dbus.Int32(1080)), signature="ii")},
            signature="sv",
        )
        # The PipeWire serial was added to the stream properties in version 6;
        # an older backend does not advertise it.
        if params.version >= 6:
            stream_properties["pipewire-serial"] = dbus.UInt64(
                params.pipewire_serial + i
            )
        streams.append((dbus.UInt32(params.node_id + i), stream_properties))

    # Windows if only windows were asked for, monitors otherwise
    window = self.source_types.get(session_handle) == 2
    audio_type = self.forced_audio_type or pick_audio_type(
        window, self.audio_types.get(session_handle, 1)
    )
    for _node_id, stream_properties in streams:
        stream_properties["source_type"] = dbus.UInt32(2 if window else 1)
        if not audio_type:
            continue
        # The portal creates the source's audio stream from it
        stream_properties["audio_source"] = audio_source(audio_type)
    response = Response(params.response, {"streams": streams})

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(response, delay=params.delay)
