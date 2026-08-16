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
MAIN_IFACE = "org.freedesktop.impl.portal.Language"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class LanguageParameters:
    delay: int
    reply_delay: int
    signal_delay: int
    response: int
    expect_close: bool
    complete_on_close: bool
    reply_before_close: int


def load(mock, parameters=None):
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "language_params")
    delay = parameters.get("delay", 1)
    mock.language_params = LanguageParameters(
        delay=delay,
        reply_delay=parameters.get("reply-delay", delay),
        signal_delay=parameters.get("signal-delay", 0),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        complete_on_close=parameters.get("complete-on-close", True),
        reply_before_close=parameters.get("reply-before-close", 0),
    )
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(VERSION),
            }
        ),
    )
    mock.language_sessions: dict[str, ImplSession] = {}
    mock.language_completed_requests: list[str] = []
    mock.language_closed_requests: list[str] = []
    mock.language_closed_sessions: list[str] = []
    mock.language_request_count = 0


def _schedule(delay, callback, *args):
    if delay > 0:
        GLib.timeout_add(delay, callback, *args)
    else:
        callback(*args)


def _schedule_signals(self, handle, session_handle, signals):
    params = self.language_params
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
    def completed(_response, _results):
        self.language_completed_requests.append(handle)
        cb_success()

    return ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        completed,
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
    params = self.language_params
    self.language_request_count += 1
    if params.expect_close and self.language_request_count > params.reply_before_close:
        request.wait_for_close(
            close_callback=lambda: self.language_closed_requests.append(request.handle),
            respond=params.complete_on_close,
        )
    else:
        request.respond(
            lambda: _response(params.response),
            delay=params.reply_delay,
        )


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="(bss)")
def GetUseCaseAvailability(self, app_id, use_case):
    logger.debug(f"GetUseCaseAvailability({app_id}, {use_case})")
    return (True, "available", "available")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="ao")
def GetCompletedRequests(self):
    return dbus.Array(self.language_completed_requests, signature="o")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="ao")
def GetClosedRequests(self):
    return dbus.Array(self.language_closed_requests, signature="o")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="ao")
def GetClosedSessions(self):
    return dbus.Array(self.language_closed_sessions, signature="o")


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

    def close_session():
        self.language_sessions.pop(session_handle, None)
        self.language_closed_sessions.append(session_handle)

    session = ImplSession(self, BUS_NAME, session_handle, app_id).export(close_session)
    self.language_sessions[session_handle] = session

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
    assert session_handle in self.language_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(self, handle, session_handle, ())


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosah(xdsss)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamResponse(
    self,
    handle,
    session_handle,
    input_json,
    media_fds,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamResponse({handle}, {session_handle}, {input_json}, {media_fds}, "
        f"{options})"
    )
    assert session_handle in self.language_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(
        self,
        handle,
        session_handle,
        (
            (
                "TokenReceived",
                "sb",
                (dbus.String("Hello"), dbus.Boolean(False)),
            ),
            (
                "TokenReceived",
                "sb",
                (dbus.String(" world"), dbus.Boolean(True)),
            ),
        ),
    )


def _guided_signals():
    tool_calls = dbus.Array(
        [
            dbus.Struct(
                (
                    dbus.String("call-1"),
                    dbus.String("tool-1"),
                    dbus.String('{"value":"mock"}'),
                ),
                signature="sss",
            )
        ],
        signature="(sss)",
    )
    return (
        (
            "GuidedSnapshotReceived",
            "sb",
            (dbus.String('{"status":"working"}'), dbus.Boolean(False)),
        ),
        (
            "GuidedToolCallsReceived",
            "a(sss)b",
            (tool_calls, dbus.Boolean(True)),
        ),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosaha(sssb)a(sss)(xds)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamRespondGuided(
    self,
    handle,
    session_handle,
    prompt,
    media_fds,
    fields,
    tools,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamRespondGuided({handle}, {session_handle}, {prompt}, {media_fds}, "
        f"{fields}, {tools}, {options})"
    )
    assert session_handle in self.language_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(
        self,
        handle,
        session_handle,
        _guided_signals(),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosaha(sss)a(sssb)a(sss)(xds)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamSubmitToolResultsGuided(
    self,
    handle,
    session_handle,
    prompt,
    media_fds,
    results,
    fields,
    tools,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"StreamSubmitToolResultsGuided({handle}, {session_handle}, {prompt}, "
        f"{media_fds}, {results}, {fields}, {tools}, {options})"
    )
    assert session_handle in self.language_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(
        self,
        handle,
        session_handle,
        _guided_signals(),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oos(s)",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def StreamEmbed(
    self,
    handle,
    session_handle,
    text,
    options,
    cb_success,
    cb_error,
):
    logger.debug(f"StreamEmbed({handle}, {session_handle}, {text}, {options})")
    assert session_handle in self.language_sessions

    request = _new_request(self, handle, cb_success, cb_error)
    _complete_request(self, request)
    _schedule_signals(
        self,
        handle,
        session_handle,
        (
            (
                "EmbeddingReceived",
                "adsb",
                (
                    dbus.Array([dbus.Double(0.25), dbus.Double(0.75)], signature="d"),
                    dbus.String("pipeline-1"),
                    dbus.Boolean(True),
                ),
            ),
        ),
    )
