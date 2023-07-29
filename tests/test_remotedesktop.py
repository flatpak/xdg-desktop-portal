# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from tests import Request, PortalTest, Session
from gi.repository import GLib

import dbus
import socket


class TestRemoteDesktop(PortalTest):
    def test_version(self):
        self.check_version(2)

    def test_remote_desktop_create_close_session(self):
        self.start_impl_portal()
        self.start_xdp()

        rd_intf = self.get_dbus_interface()
        request = Request(self.dbus_con, rd_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(self.dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = self.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == ""  # appid

        session.close()

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    def test_remote_desktop_create_session_signal_closed(self):
        params = {"force-close": 500}
        self.start_impl_portal(params=params)
        self.start_xdp()

        rd_intf = self.get_dbus_interface()
        request = Request(self.dbus_con, rd_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(self.dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = self.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == ""  # appid

        # Now expect the backend to close it

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    def test_remote_desktop_connect_to_eis(self):
        self.start_impl_portal()
        self.start_xdp()

        rd_intf = self.get_dbus_interface()
        request = Request(self.dbus_con, rd_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(self.dbus_con, response)
        request = Request(self.dbus_con, rd_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response.response == 0

        request = Request(self.dbus_con, rd_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response.response == 0

        fd = rd_intf.ConnectToEIS(session.handle, dbus.Dictionary({}, signature="sv"))
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        assert eis_socket.recv(10) == b"HELLO"

    def test_remote_desktop_connect_to_eis_fail(self):
        params = {"fail-connect-to-eis": True}
        self.start_impl_portal(params=params)
        self.start_xdp()

        rd_intf = self.get_dbus_interface()
        request = Request(self.dbus_con, rd_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(self.dbus_con, response)
        request = Request(self.dbus_con, rd_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response.response == 0

        request = Request(self.dbus_con, rd_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response.response == 0

        with self.assertRaises(dbus.exceptions.DBusException) as cm:
            _ = rd_intf.ConnectToEIS(
                session.handle, dbus.Dictionary({}, signature="sv")
            )
        assert "Purposely failing ConnectToEIS" in cm.exception.get_dbus_message()

    def test_remote_desktop_connect_to_eis_fail_notifies(self):
        self.start_impl_portal()
        self.start_xdp()

        rd_intf = self.get_dbus_interface()
        request = Request(self.dbus_con, rd_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(self.dbus_con, response)
        request = Request(self.dbus_con, rd_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response.response == 0

        request = Request(self.dbus_con, rd_intf)
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
            with self.assertRaises(dbus.exceptions.DBusException) as cm:
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
                in cm.exception.get_dbus_message()
            )
