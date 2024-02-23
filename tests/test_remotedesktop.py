# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from tests import PortalMock, Session
from gi.repository import GLib

import dbus
import pytest
import socket


@pytest.fixture
def portal_name():
    return "RemoteDesktop"


class TestRemoteDesktop:
    def test_version(self, portal_mock):
        portal_mock.check_version(2)

    def test_remote_desktop_create_close_session(self, portal_mock, appid):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == appid

        session.close()

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    @pytest.mark.parametrize("params", ({"force-close": 500},))
    def test_remote_desktop_create_session_signal_closed(self, portal_mock, appid):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == appid

        # Now expect the backend to close it

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    def test_remote_desktop_connect_to_eis(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        request = portal_mock.create_request()
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response.response == 0

        request = portal_mock.create_request()
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response.response == 0

        rd_intf = portal_mock.get_dbus_interface()
        fd = rd_intf.ConnectToEIS(session.handle, dbus.Dictionary({}, signature="sv"))
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        assert eis_socket.recv(10) == b"HELLO"

    @pytest.mark.parametrize("params", ({"fail-connect-to-eis": True},))
    def test_remote_desktop_connect_to_eis_fail(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        request = portal_mock.create_request()
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response.response == 0

        request = portal_mock.create_request()
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response.response == 0

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            rd_intf = portal_mock.get_dbus_interface()
            _ = rd_intf.ConnectToEIS(
                session.handle, dbus.Dictionary({}, signature="sv")
            )
        assert "Purposely failing ConnectToEIS" in excinfo.value.get_dbus_message()

    def test_remote_desktop_connect_to_eis_fail_notifies(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        request = portal_mock.create_request()
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response.response == 0

        request = portal_mock.create_request()
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response.response == 0

        for notifyfunc in [
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
        ]:
            with pytest.raises(dbus.exceptions.DBusException) as excinfo:
                rd_intf = portal_mock.get_dbus_interface()
                func = getattr(rd_intf, notifyfunc["name"])
                assert func is not None
                func(
                    session.handle,
                    dbus.Dictionary({}, signature="sv"),
                    *notifyfunc["args"]
                )
            # Not the best error message but...
            assert (
                "Session is not allowed to call Notify"
                in excinfo.value.get_dbus_message()
            )
