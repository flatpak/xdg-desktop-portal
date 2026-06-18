# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest


@pytest.fixture
def required_templates():
    return {"screencast": {}}


def start_stream(dbus_con, screencast_intf):
    """Drive CreateSession, SelectSources and Start, returning the Start response."""
    request = xdp.Request(dbus_con, screencast_intf)
    response = request.call("CreateSession", options={"session_handle_token": "t0"})
    assert response
    assert response.response == 0

    session = xdp.Session.from_response(dbus_con, response)

    request = xdp.Request(dbus_con, screencast_intf)
    response = request.call(
        "SelectSources",
        session_handle=session.handle,
        options={"types": dbus.UInt32(1)},
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
