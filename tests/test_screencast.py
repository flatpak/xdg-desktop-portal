# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import time

import dbus
import dbusmock
import pytest

import tests.xdp_utils as xdp


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}, "screencast": {}, "appaudiocapture": {}}


def get_backend_select_options(dbus_con):
    calls = xdp.get_mock_iface(dbus_con).GetMethodCalls("SelectSources")
    assert calls
    _timestamp, args = calls[-1]
    return args[3]


def get_streams_by_media_type(response):
    streams = {}
    for node_id, props in response.results["streams"]:
        media_type = props.get("media_type", "video")
        assert media_type not in streams
        streams[media_type] = (node_id, props)
    return streams


def get_app_audio_capture_calls(dbus_con, method):
    obj = dbus_con.get_object(
        "org.freedesktop.impl.portal.AppAudioCapture",
        "/org/freedesktop/portal/desktop",
    )
    calls = dbus.Interface(obj, dbusmock.MOCK_IFACE).GetMethodCalls(method)
    return [args for _timestamp, args in calls]


def get_audio_session(dbus_con, handle):
    return dbus.Interface(
        dbus_con.get_object("org.freedesktop.impl.portal.AppAudioCapture", handle),
        dbusmock.MOCK_IFACE,
    )


def get_audio_session_calls(dbus_con, handle, method):
    calls = get_audio_session(dbus_con, handle).GetMethodCalls(method)
    return [args for _timestamp, args in calls]


def get_create_calls(dbus_con):
    return get_app_audio_capture_calls(
        dbus_con, "CreateSystemStream"
    ) + get_app_audio_capture_calls(dbus_con, "CreateApplicationStream")


def get_property(dbus_con, name):
    properties_intf = dbus.Interface(
        xdp.get_xdp_dbus_object(dbus_con), "org.freedesktop.DBus.Properties"
    )
    return properties_intf.Get("org.freedesktop.portal.ScreenCast", name)


def select_sources_fails(dbus_con, options):
    screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
    mock_intf = xdp.get_mock_iface(dbus_con)

    request = xdp.Request(dbus_con, screencast_intf)
    response = request.call("CreateSession", options={"session_handle_token": "t0"})
    assert response
    assert response.response == 0
    session = xdp.Session.from_response(dbus_con, response)

    request = xdp.Request(dbus_con, screencast_intf)
    with pytest.raises(dbus.exceptions.DBusException) as excinfo:
        request.call("SelectSources", session_handle=session.handle, options=options)

    assert mock_intf.GetMethodCalls("SelectSources") == []
    return excinfo.value.get_dbus_name()


def start_stream(dbus_con, screencast_intf, select_options=None):
    """Drive CreateSession, SelectSources and Start, returning the Start response."""
    if select_options is None:
        select_options = {"types": dbus.UInt32(1)}

    request = xdp.Request(dbus_con, screencast_intf)
    response = request.call("CreateSession", options={"session_handle_token": "t0"})
    assert response
    assert response.response == 0

    session = xdp.Session.from_response(dbus_con, response)

    request = xdp.Request(dbus_con, screencast_intf)
    response = request.call(
        "SelectSources",
        session_handle=session.handle,
        options=select_options,
    )
    assert response
    assert response.response == 0

    request = xdp.Request(dbus_con, screencast_intf)
    response = request.call(
        "Start",
        session_handle=session.handle,
        parent_window="",
        options={},
    )
    assert response
    assert response.response == 0

    return response


class TestScreenCast:
    def test_version(self, portals, dbus_con):
        # The frontend advertises MIN(impl_version, 6); the backend here
        # implements version 6, so the portal exposes version 6.
        xdp.check_version(dbus_con, "ScreenCast", 6)

    def test_pipewire_serial_present_with_v6_backend(self, portals, dbus_con):
        # A version 6 backend returns pipewire-serial in the stream
        # properties (added in #1942). The frontend forwards the stream
        # properties to the application, so the serial is visible there.
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(dbus_con, screencast_intf)

        streams = response.results["streams"]
        assert len(streams) == 1
        _node_id, props = streams[0]
        assert "pipewire-serial" in props
        assert props["pipewire-serial"] == 133742

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"n-streams": 2}},),
    )
    def test_multiple_streams(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(dbus_con, screencast_intf)

        streams = response.results["streams"]
        assert [node_id for node_id, _props in streams] == [42, 43]

    @pytest.mark.parametrize(
        "persistence_option",
        (
            {"persist_mode": dbus.UInt32(xdp.SessionPersistenceMode.TRANSIENT)},
            {"restore_token": "restore_token"},
        ),
    )
    def test_remote_desktop_session_cannot_persist(
        self,
        portals,
        dbus_con,
        persistence_option,
    ):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        response = request.call(
            "CreateSession",
            options={"session_handle_token": "session_token0"},
        )
        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        request = xdp.Request(dbus_con, screencast_intf)

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "SelectSources",
                session_handle=session.handle,
                options=persistence_option,
            )

        e = excinfo.value
        assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        assert e.get_dbus_message() == "Remote desktop sessions cannot persist"

        mock_intf = xdp.get_mock_iface(dbus_con)
        method_calls = mock_intf.GetMethodCalls("SelectSources")
        assert len(method_calls) == 0

        # Check that the portal is still alive and responsive.
        xdp.check_version(dbus_con, "ScreenCast", 6)

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 5}},),
    )
    def test_no_pipewire_serial_with_v5_backend(self, portals, dbus_con):
        # A version 5 backend predates pipewire-serial: the portal exposes
        # version 5 and the stream carries no pipewire-serial property.
        xdp.check_version(dbus_con, "ScreenCast", 5)

        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(dbus_con, screencast_intf)

        streams = response.results["streams"]
        assert len(streams) == 1
        _node_id, props = streams[0]
        assert "pipewire-serial" not in props

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_system_audio(self, portals, dbus_con):
        assert get_property(dbus_con, "AvailableAudioTypes") == 1 | 2 | 4

        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={"types": dbus.UInt32(1), "audio_types": dbus.UInt32(2)},
        )

        assert get_backend_select_options(dbus_con)["audio_types"] == 2
        [[handle, _app_id, session_options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSession"
        )
        assert dict(session_options) == {}
        [[session_handle, options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSystemStream"
        )
        assert session_handle == handle
        assert dict(options) == {}
        streams = get_streams_by_media_type(response)
        assert set(streams) == {"video", "audio"}
        _node_id, props = streams["video"]
        audio_node_id, audio_props = streams["audio"]
        assert "audio_source" not in props
        assert audio_node_id == 77
        assert audio_props["pipewire-serial"] == 7777
        assert audio_props["mapping_id"] == props["mapping_id"]
        assert audio_props["audio_type"] == 2

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_app_audio_of_a_window(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={"types": dbus.UInt32(2), "audio_types": dbus.UInt32(4)},
        )

        assert get_backend_select_options(dbus_con)["audio_types"] == 4
        [[_handle, options]] = get_app_audio_capture_calls(
            dbus_con, "CreateApplicationStream"
        )
        assert set(options) == {"pid"}
        assert options["pid"] > 0
        streams = get_streams_by_media_type(response)
        assert set(streams) == {"video", "audio"}
        assert streams["audio"][1]["audio_type"] == 4

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_app_audio_of_a_window_in_a_remote_desktop_session(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response
        assert response.response == 0
        session = xdp.Session.from_response(dbus_con, response)

        for intf, method, options in (
            (remotedesktop_intf, "SelectDevices", {"types": dbus.UInt32(1)}),
            (
                screencast_intf,
                "SelectSources",
                {"types": dbus.UInt32(2), "audio_types": dbus.UInt32(4)},
            ),
        ):
            request = xdp.Request(dbus_con, intf)
            response = request.call(
                method, session_handle=session.handle, options=options
            )
            assert response
            assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        response = request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )
        assert response
        assert response.response == 0

        [[handle, options]] = get_app_audio_capture_calls(
            dbus_con, "CreateApplicationStream"
        )
        assert handle == session.handle
        assert set(options) == {"pid"}
        assert options["pid"] > 0

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_system_audio_of_a_window(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={"types": dbus.UInt32(2), "audio_types": dbus.UInt32(2)},
        )

        [[_handle, options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSystemStream"
        )
        assert dict(options) == {}
        assert get_streams_by_media_type(response)["audio"][1]["audio_type"] == 2

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7, "audio-types": 2}},),
    )
    def test_backend_without_app_audio(self, portals, dbus_con):
        # The backend's audio types, and None
        assert get_property(dbus_con, "AvailableAudioTypes") == 1 | 2

        options = {"types": dbus.UInt32(2), "audio_types": dbus.UInt32(4)}
        assert (
            select_sources_fails(dbus_con, options)
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

    @pytest.mark.parametrize(
        "template_params",
        (
            {"screencast": {"version": 7, "audio-type": 4}},
            {"screencast": {"version": 7, "audio-type": 8}},
        ),
    )
    def test_audio_type_not_asked_for(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response and response.response == 0
        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "SelectSources",
            session_handle=session.handle,
            options={"types": dbus.UInt32(1), "audio_types": dbus.UInt32(2)},
        )
        assert response and response.response == 0
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )

        assert response
        assert response.response == 2
        assert get_create_calls(dbus_con) == []

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_audio_stream_closed_with_session(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response and response.response == 0
        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "SelectSources",
            session_handle=session.handle,
            options={"types": dbus.UInt32(1), "audio_types": dbus.UInt32(2)},
        )
        assert response and response.response == 0
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )
        assert response and response.response == 0
        [[handle, _app_id, _options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSession"
        )
        assert handle == session.handle
        assert get_audio_session_calls(dbus_con, handle, "Close") == []

        session.close()
        xdp.wait_for(lambda: get_audio_session_calls(dbus_con, handle, "Close") == [[]])

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_session_closed_with_audio_session(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response and response.response == 0
        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "SelectSources",
            session_handle=session.handle,
            options={"types": dbus.UInt32(1), "audio_types": dbus.UInt32(2)},
        )
        assert response and response.response == 0
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )
        assert response and response.response == 0

        get_audio_session(dbus_con, session.handle).EmitSignal(
            "org.freedesktop.impl.portal.Session", "Closed", "", []
        )
        xdp.wait_for(lambda: session.closed)

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7, "delay": 500}},),
    )
    def test_audio_of_a_session_closed_during_start(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response and response.response == 0
        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "SelectSources",
            session_handle=session.handle,
            options={"types": dbus.UInt32(1), "audio_types": dbus.UInt32(2)},
        )
        assert response and response.response == 0

        # The session closes before the backend's Start replies
        session.schedule_close(100)
        request = xdp.Request(dbus_con, screencast_intf)
        request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )
        time.sleep(1)

        for handle, _app_id, _options in get_app_audio_capture_calls(
            dbus_con, "CreateSession"
        ):
            assert get_audio_session_calls(dbus_con, handle, "Close") == [[]]

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7, "audio-type": 2}},),
    )
    def test_audio_not_asked_for(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response and response.response == 0
        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "SelectSources",
            session_handle=session.handle,
            options={"types": dbus.UInt32(1)},
        )
        assert response and response.response == 0
        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )

        assert response
        assert response.response == 2
        assert get_create_calls(dbus_con) == []

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7, "n-streams": 2}},),
    )
    def test_audio_of_multiple_sources(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={
                "types": dbus.UInt32(1),
                "multiple": dbus.Boolean(True),
                "audio_types": dbus.UInt32(2),
            },
        )

        streams = response.results["streams"]
        video = [p for _n, p in streams if "media_type" not in p]
        audio = [p for _n, p in streams if p.get("media_type") == "audio"]
        assert len(video) == len(audio) == 2
        assert len(get_create_calls(dbus_con)) == 2
        assert sorted(p["mapping_id"] for p in audio) == sorted(
            p["mapping_id"] for p in video
        )
        assert len({p["mapping_id"] for p in video}) == 2

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    @pytest.mark.parametrize("types,audio_types", ((1, 4), (4, 4), (1 | 2, 4)))
    def test_uncovered_audio_types(self, portals, dbus_con, types, audio_types):
        options = {
            "types": dbus.UInt32(types),
            "audio_types": dbus.UInt32(audio_types),
        }
        assert (
            select_sources_fails(dbus_con, options)
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_exclude_self(self, portals, dbus_con):
        assert get_property(dbus_con, "AvailableExcludeSelfModes") == 1 | 2

        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={
                "types": dbus.UInt32(1),
                "audio_types": dbus.UInt32(2),
                "exclude_self": dbus.UInt32(2),
            },
        )

        assert get_backend_select_options(dbus_con)["exclude_self"] == 2
        assert set(get_streams_by_media_type(response)) == {"video", "audio"}
        # The caller's app id depends on its app info kind
        [[_handle, app_id, session_options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSession"
        )
        assert app_id
        assert dict(session_options) == {"exclude_self": True}
        [[_handle, options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSystemStream"
        )
        assert dict(options) == {}

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_exclude_self_from_own_window(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={
                "types": dbus.UInt32(2),
                "audio_types": dbus.UInt32(4),
                "exclude_self": dbus.UInt32(2),
            },
        )

        # The window's pid is this process, so with the test app info its
        # application is the caller's own: no audio stream
        assert set(get_streams_by_media_type(response)) == {"video"}
        assert get_app_audio_capture_calls(dbus_con, "CreateApplicationStream") == []
        assert get_app_audio_capture_calls(dbus_con, "CreateSession") == []

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7, "n-streams": 2}},),
    )
    def test_exclude_self_of_multiple_sources(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={
                "types": dbus.UInt32(1),
                "multiple": dbus.Boolean(True),
                "audio_types": dbus.UInt32(2),
                "exclude_self": dbus.UInt32(2),
            },
        )

        audio = [
            p for _n, p in response.results["streams"] if p.get("media_type") == "audio"
        ]
        [[handle, app_id, session_options]] = get_app_audio_capture_calls(
            dbus_con, "CreateSession"
        )
        assert app_id
        assert dict(session_options) == {"exclude_self": True}
        calls = get_app_audio_capture_calls(dbus_con, "CreateSystemStream")
        assert len(audio) == len(calls) == 2
        assert all(session_handle == handle for session_handle, _options in calls)

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    @pytest.mark.parametrize("modes", (0, 4, 1 | 2))
    def test_unavailable_exclude_self(self, portals, dbus_con, modes):
        options = {"types": dbus.UInt32(1), "exclude_self": dbus.UInt32(modes)}
        assert (
            select_sources_fails(dbus_con, options)
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

    def test_no_audio(self, portals, dbus_con):
        assert get_property(dbus_con, "AvailableAudioTypes") == 1
        assert get_property(dbus_con, "AvailableExcludeSelfModes") == 1 | 2

        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(dbus_con, screencast_intf)

        options = get_backend_select_options(dbus_con)
        assert "audio_types" not in options
        assert "exclude_self" not in options
        streams = response.results["streams"]
        assert len(streams) == 1
        assert "media_type" not in streams[0][1]

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    def test_none_audio_type(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={"types": dbus.UInt32(1), "audio_types": dbus.UInt32(1)},
        )

        assert get_backend_select_options(dbus_con)["audio_types"] == 1
        streams = response.results["streams"]
        assert len(streams) == 1
        assert "media_type" not in streams[0][1]

    def test_no_audio_from_backend_without_audio(self, portals, dbus_con):
        options = {"types": dbus.UInt32(1), "audio_types": dbus.UInt32(2)}
        assert (
            select_sources_fails(dbus_con, options)
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

    def test_exclude_self_without_audio(self, portals, dbus_con):
        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={"types": dbus.UInt32(1), "exclude_self": dbus.UInt32(2)},
        )

        assert len(response.results["streams"]) == 1
        assert get_app_audio_capture_calls(dbus_con, "CreateSession") == []

    @pytest.mark.parametrize(
        "template_params",
        ({"screencast": {"version": 7}},),
    )
    @pytest.mark.parametrize("audio_types", (0, 1 | 2, 8))
    def test_unavailable_audio_types(self, portals, dbus_con, audio_types):
        options = {"types": dbus.UInt32(1), "audio_types": dbus.UInt32(audio_types)}
        assert (
            select_sources_fails(dbus_con, options)
            == "org.freedesktop.portal.Error.InvalidArgument"
        )
