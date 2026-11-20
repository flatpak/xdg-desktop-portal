# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import dbus
import pytest

import tests.xdp_utils as xdp


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}, "screencast": {}}


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


def get_available_audio_modes(dbus_con):
    properties_intf = dbus.Interface(
        xdp.get_xdp_dbus_object(dbus_con), "org.freedesktop.DBus.Properties"
    )
    return properties_intf.Get(
        "org.freedesktop.portal.ScreenCast", "AvailableAudioModes"
    )


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
        # The frontend advertises MIN(impl_version, 7); the backend here
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
    def test_audio_with_v7_backend(self, portals, dbus_con):
        xdp.check_version(dbus_con, "ScreenCast", 7)
        assert get_available_audio_modes(dbus_con) == 7

        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        response = start_stream(
            dbus_con,
            screencast_intf,
            select_options={"types": dbus.UInt32(1), "audio_mode": dbus.UInt32(2)},
        )

        assert get_backend_select_options(dbus_con)["audio_mode"] == 2
        streams = get_streams_by_media_type(response)
        assert set(streams) == {"video", "audio"}
        _node_id, props = streams["video"]
        audio_node_id, audio_props = streams["audio"]
        assert audio_node_id == 77
        assert audio_props["mapping_id"] == props["mapping_id"]

    def test_no_audio_with_v6_backend(self, portals, dbus_con):
        assert get_available_audio_modes(dbus_con) == 0

        screencast_intf = xdp.get_portal_iface(dbus_con, "ScreenCast")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, screencast_intf)
        response = request.call("CreateSession", options={"session_handle_token": "t0"})
        assert response
        assert response.response == 0
        session = xdp.Session.from_response(dbus_con, response)

        request = xdp.Request(dbus_con, screencast_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "SelectSources",
                session_handle=session.handle,
                options={"types": dbus.UInt32(1), "audio_mode": dbus.UInt32(2)},
            )

        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )
        assert mock_intf.GetMethodCalls("SelectSources") == []
