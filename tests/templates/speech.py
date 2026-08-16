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
MAIN_IFACE = "org.freedesktop.impl.portal.Speech"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class SpeechParameters:
    delay: int
    reply_delay: int
    signal_delay: int
    response: int
    expect_close: bool


def load(mock, parameters=None):
    parameters = parameters or {}
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "speech_params")
    delay = parameters.get("delay", 1)
    mock.speech_params = SpeechParameters(
        delay=delay,
        reply_delay=parameters.get("reply-delay", delay),
        signal_delay=parameters.get("signal-delay", 0),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
    )
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary({"version": dbus.UInt32(VERSION)}),
    )
    mock.speech_sessions: dict[str, ImplSession] = {}


def _schedule(delay, callback, *args):
    if delay > 0:
        GLib.timeout_add(delay, callback, *args)
    else:
        callback(*args)


def _schedule_signals(self, handle, session_handle, signals):
    params = self.speech_params
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
            [dbus.ObjectPath(handle), dbus.ObjectPath(session_handle), *args],
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
    params = self.speech_params
    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(
            lambda: _response(params.response),
            delay=params.reply_delay,
        )


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="(bss)")
def GetUseCaseAvailability(self, app_id, use_case):
    logger.debug(f"GetUseCaseAvailability({app_id}, {use_case})")
    return dbus.Struct(
        (
            dbus.Boolean(True),
            dbus.String("available"),
            dbus.String("available"),
        ),
        signature="bss",
    )


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
        lambda: self.speech_sessions.pop(session_handle, None)
    )
    self.speech_sessions[session_handle] = session

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
    assert session_handle in self.speech_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(self, handle, session_handle, ())


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ooh(ss)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamTranscribe(
    self,
    handle,
    session_handle,
    audio_fd,
    options,
    cb_success,
    cb_error,
):
    logger.debug(f"StreamTranscribe({handle}, {session_handle}, {audio_fd}, {options})")
    assert session_handle in self.speech_sessions
    payloads = (
        (
            "TranscriptionReceived",
            "sb",
            (dbus.String("hello"), dbus.Boolean(False)),
        ),
        (
            "TranscriptionReceived",
            "sb",
            (dbus.String(""), dbus.Boolean(True)),
        ),
    )

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(self, handle, session_handle, payloads)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oos(sss)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamSynthesize(
    self,
    handle,
    session_handle,
    text,
    options,
    cb_success,
    cb_error,
):
    logger.debug(f"StreamSynthesize({handle}, {session_handle}, {text}, {options})")
    assert session_handle in self.speech_sessions
    signals = tuple(
        (
            "AudioReceived",
            "ayuusb",
            (
                dbus.ByteArray(chunk),
                dbus.UInt32(24000),
                dbus.UInt32(1),
                dbus.String("s16le"),
                dbus.Boolean(done),
            ),
        )
        for chunk, done in (
            (b"\x01\x00\x02\x00", False),
            (b"\x03\x00\x04\x00", False),
            (b"", True),
        )
    )

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(self, handle, session_handle, signals)
