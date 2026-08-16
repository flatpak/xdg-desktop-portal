# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import fcntl
import os
import signal
import xml.etree.ElementTree as ET
from contextlib import contextmanager
from itertools import count

import dbus
import pytest

import tests.xdp_utils as xdp

BUS_NAME = "org.freedesktop.portal.Desktop"
IMPL_BUS_NAME = "org.freedesktop.impl.portal.Test"
DESKTOP_PATH = "/org/freedesktop/portal/desktop"
MOCK_IFACE = "org.freedesktop.impl.portal.Mock"
LANGUAGE_IMPL_IFACE = "org.freedesktop.impl.portal.Language"
INVALID_ARGUMENT = "org.freedesktop.portal.Error.InvalidArgument"
ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"
REQUIRED_SEALS = fcntl.F_SEAL_GROW | fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK

MODALITIES = (
    pytest.param("Language", "language.summarize", id="language"),
    pytest.param("Speech", "speech.transcribe", id="speech"),
    pytest.param("Vision", "vision.describe", id="vision"),
)

VISION_STREAMS = (
    pytest.param(
        "StreamDescribe",
        "vision.describe",
        "VisionTextReceived",
        [("A cat.", False), ("", True)],
        id="describe",
    ),
    pytest.param(
        "StreamOcr",
        "vision.ocr",
        "VisionTextReceived",
        [("CAT", False), ("", True)],
        id="ocr",
    ),
    pytest.param(
        "StreamDetect",
        "vision.detect",
        "VisionDetectionsReceived",
        [([("cat", 0.9, 0.1, 0.2, 0.3, 0.4)], True)],
        id="detect",
    ),
    pytest.param(
        "StreamSegment",
        "vision.segment",
        "VisionMasksReceived",
        [([("cat", 0.8, 0.1, 0.2, 0.3, 0.4, "AA==", 2, 2)], True)],
        id="segment",
    ),
    pytest.param(
        "StreamDepth",
        "vision.depth",
        "VisionDepthReceived",
        [((2, 1, [1.0, 2.0], "meter", 1.0, 2.0), True)],
        id="depth",
    ),
)

_session_tokens = count()


@pytest.fixture
def required_templates():
    return {
        "language": {},
        "speech": {},
        "vision": {},
    }


def _plain(value):
    if isinstance(value, dbus.ByteArray):
        return bytes(value)
    if isinstance(value, dict):
        return {_plain(key): _plain(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return tuple(_plain(item) for item in value)
    if isinstance(value, list):
        if str(getattr(value, "signature", "")) == "y":
            return bytes(value)
        return [_plain(item) for item in value]
    if isinstance(value, dbus.Boolean):
        return bool(value)
    if isinstance(value, int):
        return int(value)
    if isinstance(value, float):
        return float(value)
    if isinstance(value, str):
        return str(value)
    return value


def _backend_args(mock_intf, method):
    calls = mock_intf.GetMethodCalls(method)
    assert len(calls) == 1
    return calls[0][1]


def _sender_token(bus):
    return bus.get_unique_name().lstrip(":").replace(".", "_")


def _new_bus():
    return dbus.bus.BusConnection(os.environ["DBUS_SESSION_BUS_ADDRESS"])


def _create_session(
    bus,
    portal,
    use_case,
    *,
    parent_window="",
    instructions="Session instructions",
):
    interface = xdp.get_portal_iface(bus, portal)
    token = f"{portal.lower()}_session_{next(_session_tokens)}"
    request = xdp.Request(bus, interface)
    response = request.call(
        "CreateSession",
        parent_window=parent_window,
        use_case=use_case,
        instructions=instructions,
        options={
            "session_handle_token": dbus.String(token, variant_level=1),
        },
    )

    assert response is not None
    assert response.response == 0
    session = xdp.Session.from_response(bus, response)
    return interface, session, request, token


def _close_session(session):
    session.close()
    xdp.wait_for(lambda: session.closed)


@contextmanager
def _record_signals(bus, portal, names):
    received: dict[str, list[tuple[object, ...]]] = {name: [] for name in names}
    matches = []

    for name in names:

        def on_signal(*args, _name=name):
            received[_name].append(args)

        matches.append(
            bus.add_signal_receiver(
                on_signal,
                signal_name=name,
                dbus_interface=f"org.freedesktop.portal.{portal}",
                path=DESKTOP_PATH,
                bus_name=BUS_NAME,
            )
        )

    try:
        yield received
    finally:
        for match in matches:
            match.remove()


def _payloads(events, request, session):
    assert events
    assert all(
        (str(event[0]), str(event[1])) == (request.handle, str(session.handle))
        for event in events
    )
    return [tuple(_plain(item) for item in event[2:]) for event in events]


def _assert_loading(events, request, session):
    assert _payloads(events, request, session) == [("Loading model",)]


def _new_memfd(name, contents):
    fd = os.memfd_create(name, os.MFD_ALLOW_SEALING)
    os.write(fd, contents)
    os.lseek(fd, 0, os.SEEK_SET)
    return fd


def _assert_sealed_fd(unix_fd, contents):
    fd = unix_fd.take()
    try:
        seals = fcntl.fcntl(fd, fcntl.F_GET_SEALS)
        assert seals & REQUIRED_SEALS == REQUIRED_SEALS
        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, len(contents) + 1) == contents
    finally:
        os.close(fd)


@contextmanager
def _expect_dbus_error(name):
    with pytest.raises(dbus.exceptions.DBusException) as excinfo:
        yield
    assert excinfo.value.get_dbus_name() == name


def _vision_options(method):
    options = {
        "execution_mode": dbus.String("background", variant_level=1),
    }
    if method == "StreamSegment":
        points = [(0.1, 0.2, True), (0.7, 0.8, False)]
        boxes = [(0.1, 0.2, 0.3, 0.4)]
        options["point_prompts"] = dbus.Array(
            [dbus.Struct(point, signature="ddb") for point in points],
            signature="(ddb)",
            variant_level=1,
        )
        options["box_prompts"] = dbus.Array(
            [dbus.Struct(box, signature="dddd") for box in boxes],
            signature="(dddd)",
            variant_level=1,
        )
        return options, ("background", points, boxes)
    return options, ("background",)


class TestModelPortals:
    def test_versions_and_depth_introspection(self, portals, dbus_con):
        for portal in ("Language", "Speech", "Vision"):
            xdp.check_version(dbus_con, portal, 1)

        portal = xdp.get_xdp_dbus_object(dbus_con)
        xml = portal.Introspect(dbus_interface="org.freedesktop.DBus.Introspectable")
        root = ET.fromstring(xml)
        interface = root.find("./interface[@name='org.freedesktop.portal.Vision']")
        assert interface is not None
        depth_signal = interface.find("./signal[@name='VisionDepthReceived']")
        assert depth_signal is not None
        assert [arg.get("type") for arg in depth_signal.findall("arg")] == [
            "o",
            "o",
            "(iiadsdd)",
            "b",
        ]

    @pytest.mark.parametrize("portal,use_case", MODALITIES)
    def test_supported_availability(
        self, portals, dbus_con, xdp_app_info, portal, use_case
    ):
        interface = xdp.get_portal_iface(dbus_con, portal)
        mock_intf = xdp.get_mock_iface(dbus_con)

        availability = interface.GetUseCaseAvailability(use_case, {})

        assert _plain(availability) == (True, "available", "available")
        assert _plain(_backend_args(mock_intf, "GetUseCaseAvailability")) == [
            xdp_app_info.app_id,
            use_case,
        ]

    @pytest.mark.parametrize(
        "portal,use_case",
        (
            pytest.param("Language", "speech.transcribe", id="language"),
            pytest.param("Speech", "vision.describe", id="speech"),
            pytest.param("Vision", "language.summarize", id="vision"),
        ),
    )
    def test_unsupported_availability_is_local(
        self, portals, dbus_con, portal, use_case
    ):
        interface = xdp.get_portal_iface(dbus_con, portal)
        mock_intf = xdp.get_mock_iface(dbus_con)

        availability = interface.GetUseCaseAvailability(use_case, {})

        available, code, reason = _plain(availability)
        assert available is False
        assert code == "unsupported_use_case"
        assert reason
        assert mock_intf.GetMethodCalls("GetUseCaseAvailability") == []

    @pytest.mark.parametrize(
        "portal,use_case",
        (
            pytest.param("Language", "language.unknown", id="language"),
            pytest.param("Speech", "speech.unknown", id="speech"),
            pytest.param("Vision", "vision.unknown", id="vision"),
        ),
    )
    def test_create_session_rejects_unsupported_use_case(
        self, portals, dbus_con, portal, use_case
    ):
        interface = xdp.get_portal_iface(dbus_con, portal)
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _expect_dbus_error(INVALID_ARGUMENT):
            interface.CreateSession(
                "",
                use_case,
                "",
                dbus.Dictionary({}, signature="sv"),
            )

        assert mock_intf.GetMethodCalls("CreateSession") == []

    @pytest.mark.parametrize("portal,use_case", MODALITIES)
    def test_create_session_forwarding_and_close(
        self, portals, dbus_con, xdp_app_info, portal, use_case
    ):
        mock_intf = xdp.get_mock_iface(dbus_con)
        parent_window = "x11:1"
        instructions = f"Instructions for {portal}"

        _, session, request, token = _create_session(
            dbus_con,
            portal,
            use_case,
            parent_window=parent_window,
            instructions=instructions,
        )

        expected_session = f"{DESKTOP_PATH}/session/{_sender_token(dbus_con)}/{token}"
        assert str(session.handle) == expected_session
        assert _plain(_backend_args(mock_intf, "CreateSession")) == [
            request.handle,
            expected_session,
            xdp_app_info.app_id,
            parent_window,
            use_case,
            instructions,
        ]

        _close_session(session)
        assert session.closed

    @pytest.mark.parametrize("portal,use_case", MODALITIES)
    def test_prewarm_forwards_loading(self, portals, dbus_con, portal, use_case):
        interface, session, _, _ = _create_session(dbus_con, portal, use_case)
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _record_signals(dbus_con, portal, ("ModelLoading",)) as signals:
            request = xdp.Request(dbus_con, interface)
            response = request.call(
                "Prewarm",
                session_handle=session.handle,
                options={},
            )

        assert response is not None
        assert response.response == 0
        assert _plain(_backend_args(mock_intf, "Prewarm")) == [
            request.handle,
            str(session.handle),
        ]
        _assert_loading(signals["ModelLoading"], request, session)
        _close_session(session)

    def test_session_is_owned_by_calling_connection(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.summarize"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        other_bus = _new_bus()
        other_bus.set_exit_on_disconnect(False)

        try:
            other_interface = xdp.get_portal_iface(other_bus, "Language")
            with _expect_dbus_error(ACCESS_DENIED):
                xdp.Request(other_bus, other_interface).call(
                    "Prewarm",
                    session_handle=session.handle,
                    options={},
                )

            other_session = dbus.Interface(
                other_bus.get_object(BUS_NAME, session.handle, introspect=False),
                "org.freedesktop.portal.Session",
            )
            with _expect_dbus_error(ACCESS_DENIED):
                other_session.Close()
        finally:
            other_bus.close()

        assert mock_intf.GetMethodCalls("Prewarm") == []
        response = xdp.Request(dbus_con, interface).call(
            "Prewarm",
            session_handle=session.handle,
            options={},
        )
        assert response is not None
        assert response.response == 0
        _close_session(session)

    def test_session_cannot_cross_modalities(self, portals, dbus_con):
        _, session, _, _ = _create_session(dbus_con, "Language", "language.summarize")
        speech = xdp.get_portal_iface(dbus_con, "Speech")
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _expect_dbus_error(ACCESS_DENIED):
            xdp.Request(dbus_con, speech).call(
                "Prewarm",
                session_handle=session.handle,
                options={},
            )

        assert mock_intf.GetMethodCalls("Prewarm") == []
        _close_session(session)

    def test_owner_disconnect_closes_session(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con)
        closed = []
        match = dbus_con.add_signal_receiver(
            lambda handle: closed.append(str(handle)),
            signal_name="SessionClosed",
            dbus_interface=MOCK_IFACE,
        )
        owner_bus = _new_bus()
        owner_bus.set_exit_on_disconnect(False)

        try:
            _, session, _, _ = _create_session(
                owner_bus, "Language", "language.summarize"
            )
            session_handle = str(session.handle)
            owner_bus.close()
            xdp.wait_for(lambda: session_handle in closed)
        finally:
            match.remove()

        language = xdp.get_portal_iface(dbus_con, "Language")
        with _expect_dbus_error(ACCESS_DENIED):
            xdp.Request(dbus_con, language).call(
                "Prewarm",
                session_handle=session_handle,
                options={},
            )
        assert mock_intf.GetMethodCalls("Prewarm") == []

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {
                    "language": {
                        "delay": 1,
                        "reply-delay": 40,
                        "signal-delay": 0,
                    }
                },
                id="terminal-before-backend-reply",
            ),
            pytest.param(
                {
                    "language": {
                        "delay": 20,
                        "reply-delay": 1,
                        "signal-delay": 20,
                    }
                },
                id="backend-reply-before-terminal",
            ),
        ),
    )
    def test_language_stream_response_timing_and_signal_privacy(
        self, portals, dbus_con
    ):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.translate"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        impl = dbus.Interface(
            dbus_con.get_object(IMPL_BUS_NAME, DESKTOP_PATH),
            LANGUAGE_IMPL_IFACE,
        )
        input_json = '[{"type":"input_text","text":"Hi"}]'
        media = b"immutable media"
        options = {
            "maximum_response_tokens": dbus.Int64(64, variant_level=1),
            "temperature": dbus.Double(0.25, variant_level=1),
            "source_language_hint": dbus.String("en", variant_level=1),
            "target_language_hint": dbus.String("fr", variant_level=1),
            "execution_mode": dbus.String("background", variant_level=1),
        }
        other_bus = _new_bus()
        other_bus.set_exit_on_disconnect(False)

        try:
            with (
                _record_signals(
                    dbus_con, "Language", ("ModelLoading", "TokenReceived")
                ) as signals,
                _record_signals(
                    other_bus, "Language", ("ModelLoading", "TokenReceived")
                ) as other_signals,
            ):
                fd = _new_memfd("language-media", media)
                try:
                    request = xdp.Request(dbus_con, interface)
                    response = request.call(
                        "StreamResponse",
                        session_handle=session.handle,
                        input_json=input_json,
                        media_fds=dbus.Array([dbus.types.UnixFd(fd)], signature="h"),
                        options=options,
                    )
                finally:
                    os.close(fd)
                other_bus.get_object(BUS_NAME, DESKTOP_PATH).Ping(
                    dbus_interface="org.freedesktop.DBus.Peer"
                )
                assert other_signals["ModelLoading"] == []
                assert other_signals["TokenReceived"] == []
        finally:
            other_bus.close()

        assert response is not None
        assert response.response == 0
        assert request.handle in [str(path) for path in impl.GetCompletedRequests()]
        args = _backend_args(mock_intf, "StreamResponse")
        assert _plain(args[:3]) == [request.handle, str(session.handle), input_json]
        assert len(args[3]) == 1
        _assert_sealed_fd(args[3][0], media)
        assert _plain(args[4]) == (64, 0.25, "en", "fr", "background")
        _assert_loading(signals["ModelLoading"], request, session)
        assert _payloads(signals["TokenReceived"], request, session) == [
            ("Hello", False),
            (" world", True),
        ]
        _close_session(session)

    @pytest.mark.parametrize(
        "method",
        ("StreamRespondGuided", "StreamSubmitToolResultsGuided"),
    )
    def test_language_guided_streams(self, portals, dbus_con, method):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.analyze"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        prompt = "Analyze this text"
        fields_value = [("summary", "string", "Short summary", True)]
        tools_value = [("tool-1", "Lookup", '{"type":"object"}')]
        results_value = [("call-1", "lookup result", '{"ok":true}')]
        fields = dbus.Array(
            [dbus.Struct(value, signature="sssb") for value in fields_value],
            signature="(sssb)",
        )
        tools = dbus.Array(
            [dbus.Struct(value, signature="sss") for value in tools_value],
            signature="(sss)",
        )
        results = dbus.Array(
            [dbus.Struct(value, signature="sss") for value in results_value],
            signature="(sss)",
        )
        options = {
            "maximum_response_tokens": dbus.Int64(32, variant_level=1),
            "temperature": dbus.Double(0.1, variant_level=1),
            "execution_mode": dbus.String("background", variant_level=1),
        }
        names = (
            "ModelLoading",
            "GuidedSnapshotReceived",
            "GuidedToolCallsReceived",
        )

        with _record_signals(dbus_con, "Language", names) as signals:
            request = xdp.Request(dbus_con, interface)
            kwargs = {
                "session_handle": session.handle,
                "prompt": prompt,
                "media_fds": dbus.Array([], signature="h"),
            }
            if method == "StreamSubmitToolResultsGuided":
                kwargs["results"] = results
            kwargs["fields"] = fields
            kwargs["tools"] = tools
            kwargs["options"] = options
            response = request.call(method, **kwargs)

        assert response is not None
        assert response.response == 0
        args = _backend_args(mock_intf, method)
        assert _plain(args[:4]) == [
            request.handle,
            str(session.handle),
            prompt,
            [],
        ]
        expected_tail = [fields_value, tools_value, (32, 0.1, "background")]
        if method == "StreamSubmitToolResultsGuided":
            expected_tail.insert(0, results_value)
        assert _plain(args[4:]) == expected_tail
        _assert_loading(signals["ModelLoading"], request, session)
        assert _payloads(signals["GuidedSnapshotReceived"], request, session) == [
            ('{"status":"working"}', False)
        ]
        assert _payloads(signals["GuidedToolCallsReceived"], request, session) == [
            ([("call-1", "tool-1", '{"value":"mock"}')], True)
        ]
        _close_session(session)

    def test_language_stream_embed(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.embed"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _record_signals(
            dbus_con, "Language", ("ModelLoading", "EmbeddingReceived")
        ) as signals:
            request = xdp.Request(dbus_con, interface)
            response = request.call(
                "StreamEmbed",
                session_handle=session.handle,
                text="Embed this",
                options={
                    "execution_mode": dbus.String("background", variant_level=1),
                },
            )

        assert response is not None
        assert response.response == 0
        assert _plain(_backend_args(mock_intf, "StreamEmbed")) == [
            request.handle,
            str(session.handle),
            "Embed this",
            ("background",),
        ]
        _assert_loading(signals["ModelLoading"], request, session)
        assert _payloads(signals["EmbeddingReceived"], request, session) == [
            ([0.25, 0.75], "pipeline-1", True)
        ]
        _close_session(session)

    def test_speech_stream_transcribe(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Speech", "speech.transcribe"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        audio = b"mock f32le audio"

        with _record_signals(
            dbus_con, "Speech", ("ModelLoading", "TranscriptionReceived")
        ) as signals:
            fd = _new_memfd("speech-audio", audio)
            try:
                request = xdp.Request(dbus_con, interface)
                response = request.call(
                    "StreamTranscribe",
                    session_handle=session.handle,
                    audio_fd=dbus.types.UnixFd(fd),
                    options={
                        "source_language_hint": dbus.String("de", variant_level=1),
                        "execution_mode": dbus.String("background", variant_level=1),
                    },
                )
            finally:
                os.close(fd)

        assert response is not None
        assert response.response == 0
        args = _backend_args(mock_intf, "StreamTranscribe")
        assert _plain(args[:2]) == [request.handle, str(session.handle)]
        _assert_sealed_fd(args[2], audio)
        assert _plain(args[3]) == ("de", "background")
        _assert_loading(signals["ModelLoading"], request, session)
        assert _payloads(signals["TranscriptionReceived"], request, session) == [
            ("hello", False),
            ("", True),
        ]
        _close_session(session)

    def test_speech_stream_synthesize(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Speech", "speech.synthesize"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _record_signals(
            dbus_con, "Speech", ("ModelLoading", "AudioReceived")
        ) as signals:
            request = xdp.Request(dbus_con, interface)
            response = request.call(
                "StreamSynthesize",
                session_handle=session.handle,
                text="Hello.",
                options={
                    "voice_id": dbus.String("voice-1", variant_level=1),
                    "language_hint": dbus.String("en", variant_level=1),
                    "execution_mode": dbus.String("background", variant_level=1),
                },
            )

        assert response is not None
        assert response.response == 0
        assert _plain(_backend_args(mock_intf, "StreamSynthesize")) == [
            request.handle,
            str(session.handle),
            "Hello.",
            ("voice-1", "en", "background"),
        ]
        _assert_loading(signals["ModelLoading"], request, session)
        assert _payloads(signals["AudioReceived"], request, session) == [
            (b"\x01\x00\x02\x00", 24000, 1, "s16le", False),
            (b"\x03\x00\x04\x00", 24000, 1, "s16le", False),
            (b"", 24000, 1, "s16le", True),
        ]
        _close_session(session)

    @pytest.mark.parametrize(
        "method,use_case,signal_name,expected_payloads", VISION_STREAMS
    )
    def test_vision_streams(
        self,
        portals,
        dbus_con,
        method,
        use_case,
        signal_name,
        expected_payloads,
    ):
        interface, session, _, _ = _create_session(dbus_con, "Vision", use_case)
        mock_intf = xdp.get_mock_iface(dbus_con)
        image = b"\x89PNG\r\n\x1a\nmock image"
        instructions = f"{method} instructions"
        options, expected_options = _vision_options(method)

        with _record_signals(
            dbus_con, "Vision", ("ModelLoading", signal_name)
        ) as signals:
            fd = _new_memfd("vision-image", image)
            try:
                request = xdp.Request(dbus_con, interface)
                response = request.call(
                    method,
                    session_handle=session.handle,
                    image_fd=dbus.types.UnixFd(fd),
                    instructions=instructions,
                    options=options,
                )
            finally:
                os.close(fd)

        assert response is not None
        assert response.response == 0
        args = _backend_args(mock_intf, method)
        assert _plain(args[:2]) == [request.handle, str(session.handle)]
        _assert_sealed_fd(args[2], image)
        assert _plain(args[3]) == instructions
        assert _plain(args[4]) == expected_options
        _assert_loading(signals["ModelLoading"], request, session)
        assert _payloads(signals[signal_name], request, session) == expected_payloads
        _close_session(session)

    @pytest.mark.parametrize(
        "method,wrong_use_case",
        (
            pytest.param("StreamDescribe", "vision.ocr", id="describe"),
            pytest.param("StreamOcr", "vision.describe", id="ocr"),
            pytest.param("StreamDetect", "vision.describe", id="detect"),
            pytest.param("StreamSegment", "vision.describe", id="segment"),
            pytest.param("StreamDepth", "vision.describe", id="depth"),
        ),
    )
    def test_vision_streams_require_exact_use_case(
        self, portals, dbus_con, method, wrong_use_case
    ):
        interface, session, _, _ = _create_session(dbus_con, "Vision", wrong_use_case)
        mock_intf = xdp.get_mock_iface(dbus_con)
        fd = _new_memfd("wrong-vision-use-case", b"image")
        try:
            with _expect_dbus_error(INVALID_ARGUMENT):
                xdp.Request(dbus_con, interface).call(
                    method,
                    session_handle=session.handle,
                    image_fd=dbus.types.UnixFd(fd),
                    instructions="",
                    options={},
                )
        finally:
            os.close(fd)

        assert mock_intf.GetMethodCalls(method) == []
        _close_session(session)

    def test_invalid_execution_mode(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.embed"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _expect_dbus_error(INVALID_ARGUMENT):
            xdp.Request(dbus_con, interface).call(
                "StreamEmbed",
                session_handle=session.handle,
                text="text",
                options={
                    "execution_mode": dbus.String("batch", variant_level=1),
                },
            )

        assert mock_intf.GetMethodCalls("StreamEmbed") == []
        _close_session(session)

    @pytest.mark.parametrize(
        "portal,use_case,method,kwargs",
        (
            pytest.param(
                "Language",
                "language.summarize",
                "StreamEmbed",
                {"text": "text"},
                id="language",
            ),
            pytest.param(
                "Speech",
                "speech.transcribe",
                "StreamSynthesize",
                {"text": "text"},
                id="speech",
            ),
        ),
    )
    def test_stream_method_rejects_wrong_use_case(
        self, portals, dbus_con, portal, use_case, method, kwargs
    ):
        interface, session, _, _ = _create_session(dbus_con, portal, use_case)
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _expect_dbus_error(INVALID_ARGUMENT):
            xdp.Request(dbus_con, interface).call(
                method,
                session_handle=session.handle,
                **kwargs,
                options={},
            )

        assert mock_intf.GetMethodCalls(method) == []
        _close_session(session)

    def test_segment_rejects_wrong_prompt_type(self, portals, dbus_con):
        interface, session, _, _ = _create_session(dbus_con, "Vision", "vision.segment")
        mock_intf = xdp.get_mock_iface(dbus_con)
        fd = _new_memfd("segment-wrong-option", b"image")
        try:
            with _expect_dbus_error(INVALID_ARGUMENT):
                xdp.Request(dbus_con, interface).call(
                    "StreamSegment",
                    session_handle=session.handle,
                    image_fd=dbus.types.UnixFd(fd),
                    instructions="",
                    options={
                        "point_prompts": dbus.String("not an array", variant_level=1),
                    },
                )
        finally:
            os.close(fd)

        assert mock_intf.GetMethodCalls("StreamSegment") == []
        _close_session(session)

    def test_synthesize_rejects_empty_text(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Speech", "speech.synthesize"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)

        with _expect_dbus_error(INVALID_ARGUMENT):
            xdp.Request(dbus_con, interface).call(
                "StreamSynthesize",
                session_handle=session.handle,
                text="",
                options={},
            )

        assert mock_intf.GetMethodCalls("StreamSynthesize") == []
        _close_session(session)

    def test_stream_rejects_unsealable_file(self, portals, dbus_con, tmp_path):
        interface, session, _, _ = _create_session(
            dbus_con, "Vision", "vision.describe"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        path = tmp_path / "image.png"
        path.write_bytes(b"image")
        fd = os.open(path, os.O_RDONLY)
        try:
            with _expect_dbus_error(INVALID_ARGUMENT):
                xdp.Request(dbus_con, interface).call(
                    "StreamDescribe",
                    session_handle=session.handle,
                    image_fd=dbus.types.UnixFd(fd),
                    instructions="",
                    options={},
                )
        finally:
            os.close(fd)

        assert mock_intf.GetMethodCalls("StreamDescribe") == []
        _close_session(session)

    @pytest.mark.parametrize("portal,use_case", MODALITIES)
    @pytest.mark.parametrize(
        "template_params,expected_response,error_name,error_message",
        (
            pytest.param(
                {
                    "language": {"response": 1},
                    "speech": {"response": 1},
                    "vision": {"response": 1},
                },
                1,
                "org.freedesktop.portal.Error.Cancelled",
                "Cancelled by mock backend",
                id="cancelled",
            ),
            pytest.param(
                {
                    "language": {"response": 2},
                    "speech": {"response": 2},
                    "vision": {"response": 2},
                },
                2,
                "org.freedesktop.portal.Error.Failed",
                "Mock backend error",
                id="failed",
            ),
        ),
    )
    def test_backend_error_response_mapping(
        self,
        portals,
        dbus_con,
        portal,
        use_case,
        expected_response,
        error_name,
        error_message,
    ):
        interface = xdp.get_portal_iface(dbus_con, portal)
        mock_intf = xdp.get_mock_iface(dbus_con)
        session_closed = []
        match = dbus_con.add_signal_receiver(
            lambda handle: session_closed.append(str(handle)),
            signal_name="SessionClosed",
            dbus_interface=MOCK_IFACE,
        )
        request = xdp.Request(dbus_con, interface)

        try:
            response = request.call(
                "CreateSession",
                parent_window="",
                use_case=use_case,
                instructions="",
                options={
                    "session_handle_token": dbus.String(
                        "error_session", variant_level=1
                    ),
                },
            )
            backend_session = str(_backend_args(mock_intf, "CreateSession")[1])
            xdp.wait_for(lambda: backend_session in session_closed)
        finally:
            match.remove()

        assert response is not None
        assert response.response == expected_response
        assert _plain(response.results) == {
            "error": error_message,
            "error_name": error_name,
        }
        assert "GDBus.Error" not in str(response.results["error"])
        assert error_name not in str(response.results["error"])

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "language": {
                    "expect-close": True,
                    "complete-on-close": False,
                }
            },
        ),
    )
    def test_request_close_during_create_session(self, portals, dbus_con):
        interface = xdp.get_portal_iface(dbus_con, "Language")
        mock_intf = xdp.get_mock_iface(dbus_con)
        request_closed = []
        session_closed = []
        request_match = dbus_con.add_signal_receiver(
            lambda handle: request_closed.append(str(handle)),
            signal_name="RequestClosed",
            dbus_interface=MOCK_IFACE,
        )
        session_match = dbus_con.add_signal_receiver(
            lambda handle: session_closed.append(str(handle)),
            signal_name="SessionClosed",
            dbus_interface=MOCK_IFACE,
        )

        request = xdp.Request(dbus_con, interface)
        try:
            request.schedule_close(100)
            response = request.call(
                "CreateSession",
                parent_window="",
                use_case="language.summarize",
                instructions="",
                options={
                    "session_handle_token": dbus.String(
                        "tentative_session", variant_level=1
                    ),
                },
            )
            args = _backend_args(mock_intf, "CreateSession")
            backend_session = str(args[1])
            xdp.wait_for(lambda: backend_session in session_closed)
        finally:
            request_match.remove()
            session_match.remove()

        assert response is None
        assert request.response is None
        assert request.closed
        assert request_closed == [request.handle]
        assert session_closed == [backend_session]

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "language": {
                    "expect-close": True,
                    "complete-on-close": False,
                    "reply-before-close": 1,
                }
            },
        ),
    )
    def test_request_close_cancels_fd_stream(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.summarize"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        impl = dbus.Interface(
            dbus_con.get_object(IMPL_BUS_NAME, DESKTOP_PATH),
            LANGUAGE_IMPL_IFACE,
        )
        media = b"retained immutable media"
        fd = _new_memfd("cancelled-language-media", media)

        try:
            request = xdp.Request(dbus_con, interface)
            request.schedule_close(100)
            response = request.call(
                "StreamResponse",
                session_handle=session.handle,
                input_json='[{"type":"input_text","text":"cancel"}]',
                media_fds=dbus.Array([dbus.types.UnixFd(fd)], signature="h"),
                options={},
            )
        finally:
            os.close(fd)

        assert response is None
        assert request.closed
        args = _backend_args(mock_intf, "StreamResponse")
        _assert_sealed_fd(args[3][0], media)
        assert [str(path) for path in impl.GetClosedRequests()] == [request.handle]
        _close_session(session)

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {
                    "language": {
                        "delay": 1,
                        "reply-delay": 500,
                        "signal-delay": 500,
                    }
                },
                id="session-close-before-backend-reply",
            ),
            pytest.param(
                {
                    "language": {
                        "delay": 1,
                        "reply-delay": 1,
                        "signal-delay": 500,
                    }
                },
                id="backend-reply-before-session-close",
            ),
        ),
    )
    def test_session_close_cancels_inflight_stream(self, portals, dbus_con):
        interface, session, _, _ = _create_session(
            dbus_con, "Language", "language.summarize"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        session.schedule_close(25)

        with _record_signals(dbus_con, "Language", ("TokenReceived",)) as signals:
            request = xdp.Request(dbus_con, interface)
            response = request.call(
                "StreamResponse",
                session_handle=session.handle,
                input_json='[{"type":"input_text","text":"cancel"}]',
                media_fds=dbus.Array([], signature="h"),
                options={},
            )
            xdp.wait(550)

        assert response is not None
        assert response.response == 1
        assert "error" in response.results
        assert session.closed
        assert signals["TokenReceived"] == []
        assert len(mock_intf.GetMethodCalls("StreamResponse")) == 1

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "language": {
                    "expect-close": True,
                    "complete-on-close": False,
                }
            },
        ),
    )
    def test_shutdown_with_inflight_model_call(
        self, portals, dbus_con, xdg_desktop_portal
    ):
        interface = xdp.get_portal_iface(dbus_con, "Language")
        mock_intf = xdp.get_mock_iface(dbus_con)
        impl = dbus.Interface(
            dbus_con.get_object(IMPL_BUS_NAME, DESKTOP_PATH),
            LANGUAGE_IMPL_IFACE,
        )

        interface.CreateSession(
            "",
            "language.summarize",
            "shutdown test",
            dbus.Dictionary(
                {
                    "handle_token": dbus.String("shutdown_request", variant_level=1),
                    "session_handle_token": dbus.String(
                        "shutdown_session", variant_level=1
                    ),
                },
                signature="sv",
            ),
            reply_handler=lambda *_args: None,
            error_handler=lambda *_args: None,
        )

        xdp.wait_for(lambda: len(mock_intf.GetMethodCalls("CreateSession")) == 1)
        backend_request, backend_session = [
            str(value) for value in _backend_args(mock_intf, "CreateSession")[:2]
        ]
        assert xdg_desktop_portal.poll() is None

        xdg_desktop_portal.send_signal(signal.SIGHUP)
        xdg_desktop_portal.communicate(timeout=10)

        assert xdg_desktop_portal.returncode == 0
        xdp.wait_for(
            lambda: (
                backend_request in [str(path) for path in impl.GetClosedRequests()]
                and backend_session in [str(path) for path in impl.GetClosedSessions()]
            )
        )
        assert [str(path) for path in impl.GetClosedRequests()] == [backend_request]
        assert [str(path) for path in impl.GetClosedSessions()] == [backend_session]
