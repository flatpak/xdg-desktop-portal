# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest
import os


@pytest.fixture
def required_templates():
    return {
        "Clipboard": {},
        "RemoteDesktop": {"force-clipboard-enabled": True},
    }


class TestClipboard:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Clipboard", 1)

    def start_session(self, dbus_con):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        create_session_request = xdp.Request(dbus_con, remotedesktop_intf)
        create_session_response = create_session_request.call(
            "CreateSession", options={"session_handle_token": "1234"}
        )
        assert create_session_response
        assert create_session_response.response == 0
        assert str(create_session_response.results["session_handle"])

        session = xdp.Session.from_response(dbus_con, create_session_response)

        clipboard_intf.RequestClipboard(session.handle, {})

        start_session_request = xdp.Request(dbus_con, remotedesktop_intf)
        start_session_response = start_session_request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )

        assert start_session_response
        assert start_session_response.response == 0

        return (session, start_session_response.results.get("clipboard_enabled"))

    def test_request_clipboard_and_start_session(self, portals, dbus_con):
        _, clipboard_enabled = self.start_session(dbus_con)

        assert clipboard_enabled

    @pytest.mark.parametrize(
        "template_params", ({"RemoteDesktop": {"force-clipboard-enabled": False}},)
    )
    def test_clipboard_checks_clipboard_enabled(self, portals, dbus_con):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, clipboard_enabled = self.start_session(dbus_con)

        assert not clipboard_enabled

        with pytest.raises(dbus.exceptions.DBusException):
            clipboard_intf.SetSelection(session.handle, {})

    def test_clipboard_set_selection(self, portals, dbus_con):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, _ = self.start_session(dbus_con)

        clipboard_intf.SetSelection(session.handle, {})

    def test_clipboard_selection_write(self, portals, dbus_con):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, _ = self.start_session(dbus_con)

        fd_object: dbus.types.UnixFd = clipboard_intf.SelectionWrite(
            session.handle, 1234
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        bytes_written = os.write(fd, b"Clipboard")
        assert bytes_written > 0

        clipboard_intf.SelectionWriteDone(session.handle, 1234, True)

    def test_clipboard_selection_read(self, portals, dbus_con):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, _ = self.start_session(dbus_con)

        fd_object: dbus.types.UnixFd = clipboard_intf.SelectionRead(
            session.handle, "mimetype"
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        clipboard_contents = os.read(fd, 1000)
        assert str(clipboard_contents)
