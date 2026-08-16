# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from dataclasses import dataclass

import dbus
import dbus.service
from gi.repository import GLib

from tests.templates.xdp_utils import ImplRequest, ImplSession, Response, init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Vision"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class VisionParameters:
    delay: int
    reply_delay: int
    signal_delay: int
    response: int
    expect_close: bool


def load(mock, parameters=None):
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "vision_params")
    delay = parameters.get("delay", 0)
    mock.vision_params = VisionParameters(
        delay=delay,
        reply_delay=parameters.get("reply-delay", delay),
        signal_delay=parameters.get("signal-delay", 0),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
    )
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(VERSION),
            }
        ),
    )
    mock.vision_sessions: dict[str, ImplSession] = {}


def _schedule(delay, callback, *args):
    if delay > 0:
        GLib.timeout_add(delay, callback, *args)
    else:
        callback(*args)


def _schedule_signals(self, handle, session_handle, signals):
    params = self.vision_params
    signals = (
        ("ModelLoading", "s", (dbus.String("Loading model"),)),
        *signals,
    )

    def emit(index):
        name, signature, args = signals[index]
        logger.debug(f"Signal {name}({handle}, {session_handle}, {args})")
        self.EmitSignal(
            MAIN_IFACE,
            name,
            f"oo{signature}",
            [handle, session_handle, *args],
        )
        if index + 1 < len(signals):
            _schedule(params.delay, emit, index + 1)
        return GLib.SOURCE_REMOVE

    _schedule(params.signal_delay, emit, 0)


def _new_request(self, handle, cb_success, cb_error):
    return ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        lambda _response, _results: cb_success(),
        cb_error,
    )


def _response(response):
    if response != 0:
        cancelled = response == 1
        raise dbus.exceptions.DBusException(
            "Cancelled by mock backend" if cancelled else "Mock backend error",
            name=(
                "org.freedesktop.portal.Error.Cancelled"
                if cancelled
                else "org.freedesktop.portal.Error.Failed"
            ),
        )
    return Response(0, {})


def _complete_request(self, request):
    params = self.vision_params
    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(
            lambda: _response(params.response),
            delay=params.reply_delay,
        )


def _stream(self, handle, session_handle, signals, cb_success, cb_error):
    assert session_handle in self.vision_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _schedule_signals(self, handle, session_handle, signals)
    _complete_request(self, request)


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="(bss)")
def GetUseCaseAvailability(self, app_id, use_case):
    logger.debug(f"GetUseCaseAvailability({app_id}, {use_case})")
    return (True, "available", "available")


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oossss",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateSession(
    self,
    handle,
    session_handle,
    app_id,
    parent_window,
    use_case,
    instructions,
    cb_success,
    cb_error,
):
    logger.debug(
        f"CreateSession({handle}, {session_handle}, {app_id}, {parent_window}, "
        f"{use_case}, {instructions})"
    )

    session = ImplSession(self, BUS_NAME, session_handle, app_id).export(
        lambda: self.vision_sessions.pop(session_handle, None)
    )
    self.vision_sessions[session_handle] = session

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oo",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def Prewarm(self, handle, session_handle, cb_success, cb_error):
    logger.debug(f"Prewarm({handle}, {session_handle})")
    assert session_handle in self.vision_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _schedule_signals(self, handle, session_handle, ())
    _complete_request(self, request)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oohs(s)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamDescribe(
    self,
    handle,
    session_handle,
    image_fd,
    instructions,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamDescribe({handle}, {session_handle}, {image_fd}, {instructions}, "
        f"{options})"
    )
    _stream(
        self,
        handle,
        session_handle,
        (
            (
                "VisionTextReceived",
                "sb",
                (dbus.String("A cat."), dbus.Boolean(False)),
            ),
            (
                "VisionTextReceived",
                "sb",
                (dbus.String(""), dbus.Boolean(True)),
            ),
        ),
        cb_success,
        cb_error,
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oohs(s)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamOcr(
    self,
    handle,
    session_handle,
    image_fd,
    instructions,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamOcr({handle}, {session_handle}, {image_fd}, {instructions}, {options})"
    )
    _stream(
        self,
        handle,
        session_handle,
        (
            (
                "VisionTextReceived",
                "sb",
                (dbus.String("CAT"), dbus.Boolean(False)),
            ),
            (
                "VisionTextReceived",
                "sb",
                (dbus.String(""), dbus.Boolean(True)),
            ),
        ),
        cb_success,
        cb_error,
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oohs(s)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamDetect(
    self,
    handle,
    session_handle,
    image_fd,
    instructions,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamDetect({handle}, {session_handle}, {image_fd}, {instructions}, "
        f"{options})"
    )
    detections = dbus.Array(
        [
            dbus.Struct(
                (
                    dbus.String("cat"),
                    dbus.Double(0.9),
                    dbus.Double(0.1),
                    dbus.Double(0.2),
                    dbus.Double(0.3),
                    dbus.Double(0.4),
                ),
                signature="sddddd",
            )
        ],
        signature="(sddddd)",
    )
    _stream(
        self,
        handle,
        session_handle,
        (
            (
                "VisionDetectionsReceived",
                "a(sddddd)b",
                (detections, dbus.Boolean(True)),
            ),
        ),
        cb_success,
        cb_error,
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oohs(sa(ddb)a(dddd))",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamSegment(
    self,
    handle,
    session_handle,
    image_fd,
    instructions,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamSegment({handle}, {session_handle}, {image_fd}, {instructions}, "
        f"{options})"
    )
    masks = dbus.Array(
        [
            dbus.Struct(
                (
                    dbus.String("cat"),
                    dbus.Double(0.8),
                    dbus.Double(0.1),
                    dbus.Double(0.2),
                    dbus.Double(0.3),
                    dbus.Double(0.4),
                    dbus.String("AA=="),
                    dbus.Int32(2),
                    dbus.Int32(2),
                ),
                signature="sdddddsii",
            )
        ],
        signature="(sdddddsii)",
    )
    _stream(
        self,
        handle,
        session_handle,
        (
            (
                "VisionMasksReceived",
                "a(sdddddsii)b",
                (masks, dbus.Boolean(True)),
            ),
        ),
        cb_success,
        cb_error,
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oohs(s)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamDepth(
    self,
    handle,
    session_handle,
    image_fd,
    instructions,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamDepth({handle}, {session_handle}, {image_fd}, {instructions}, "
        f"{options})"
    )
    depth = dbus.Struct(
        (
            dbus.Int32(2),
            dbus.Int32(1),
            dbus.Array([dbus.Double(1.0), dbus.Double(2.0)], signature="d"),
            dbus.String("meter"),
            dbus.Double(1.0),
            dbus.Double(2.0),
        ),
        signature="iiadsdd",
    )
    _stream(
        self,
        handle,
        session_handle,
        (
            (
                "VisionDepthReceived",
                "(iiadsdd)b",
                (depth, dbus.Boolean(True)),
            ),
        ),
        cb_success,
        cb_error,
    )
