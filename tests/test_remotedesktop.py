# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
import socket
from typing import List, Dict, Any


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}}


class TestRemoteDesktop:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "RemoteDesktop", 2)

    def test_create_close_session(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        # assert args[2] == ""  # appid, not necessary empty

        session.close()
        xdp.wait_for(lambda: session.closed)

    @pytest.mark.parametrize("token", ("Invalid-Token&", "", "/foo"))
    def test_remote_desktop_create_session_invalid(self, portals, dbus_con, token):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {"session_handle_token": token}

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call("CreateSession", options=options)

        e = excinfo.value
        assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        assert "Invalid token" in e.get_dbus_message()

    @pytest.mark.parametrize(
        "template_params", ({"remotedesktop": {"force-close": 500}},)
    )
    def test_create_session_signal_closed(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        # assert args[2] == ""  # appid, not necessary empty

        # Now expect the backend to close it
        xdp.wait_for(lambda: session.closed)

    def test_connect_to_eis(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response
        assert response.response == 0

        fd = remotedesktop_intf.ConnectToEIS(
            session.handle,
            dbus.Dictionary({}, signature="sv"),
        )
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        assert eis_socket.recv(10) == b"HELLO"

    @pytest.mark.parametrize(
        "template_params", ({"remotedesktop": {"fail-connect-to-eis": True}},)
    )
    def test_connect_to_eis_fail(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response
        assert response.response == 0

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            _ = remotedesktop_intf.ConnectToEIS(
                session.handle, dbus.Dictionary({}, signature="sv")
            )
        assert "Purposely failing ConnectToEIS" in excinfo.value.get_dbus_message()

    def test_connect_to_eis_fail_notifies(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response
        assert response.response == 0

        notifyfuncs: List[Dict[str, Any]] = [
            {"name": "NotifyPointerMotion", "args": (1, 2)},
            {"name": "NotifyPointerMotionAbsolute", "args": (0, 1, 2)},
            {"name": "NotifyPointerButton", "args": (1, 1)},
            {"name": "NotifyPointerAxis", "args": (1, 1)},
            {"name": "NotifyPointerAxisDiscrete", "args": (1, 1)},
            {"name": "NotifyKeyboardKeycode", "args": (1, 1)},
            {"name": "NotifyKeyboardKeysym", "args": (1, 1)},
            {"name": "NotifyTouchDown", "args": (0, 0, 1, 1)},
            {"name": "NotifyTouchMotion", "args": (0, 0, 1, 1)},
            {"name": "NotifyTouchUp", "args": (0,)},
        ]
        for notifyfunc in notifyfuncs:
            with pytest.raises(dbus.exceptions.DBusException) as excinfo:
                func = getattr(remotedesktop_intf, notifyfunc["name"])
                assert func is not None
                func(
                    session.handle,
                    dbus.Dictionary({}, signature="sv"),
                    *notifyfunc["args"],
                )
            # Not the best error message but...
            assert (
                "Session is not allowed to call Notify"
                in excinfo.value.get_dbus_message()
            )
