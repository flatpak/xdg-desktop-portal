# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from tests import Request, PortalTest, Session
from gi.repository import GLib

import dbus
import time


class TestRemoteDesktop(PortalTest):
    def test_version(self):
        self.check_version(1)

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
