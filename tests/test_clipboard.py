# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from logging import debug
from tests import Request, PortalTest, Session
from gi.repository import GLib
import dbus
import os


class TestClipboard(PortalTest):
    def test_version(self):
        self.check_version(1)

    def start_session(self, params={}):
        self.start_impl_portal(params=params)
        self.add_template(portal="RemoteDesktop", params=params)
        self.start_xdp()

        remote_desktop_interface = self.get_dbus_interface("RemoteDesktop")
        clipboard_interface = self.get_dbus_interface()

        create_session_request = Request(self.dbus_con, remote_desktop_interface)
        create_session_response = create_session_request.call(
            "CreateSession", options={"session_handle_token": "1234"}
        )
        assert create_session_response.response == 0
        assert str(create_session_response.results["session_handle"])

        session = Session.from_response(self.dbus_con, create_session_response)

        clipboard_interface.RequestClipboard(session.handle, {})

        start_session_request = Request(self.dbus_con, remote_desktop_interface)
        start_session_response = start_session_request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )

        assert start_session_response.response == 0

        return (session, start_session_response.results.get("clipboard_enabled"))

    def test_request_clipboard_and_start_session(self):
        params = {"force-clipboard-enabled": True}
        _, clipboard_enabled = self.start_session(params)

        assert clipboard_enabled

    def test_clipboard_checks_clipboard_enabled(self):
        session, clipboard_enabled = self.start_session()
        clipboard_interface = self.get_dbus_interface()

        self.assertFalse(clipboard_enabled)

        with self.assertRaises(dbus.exceptions.DBusException):
            clipboard_interface.SetSelection(session.handle, {})

    def test_clipboard_set_selection(self):
        params = {"force-clipboard-enabled": True}
        session, _ = self.start_session(params)
        clipboard_interface = self.get_dbus_interface()

        clipboard_interface.SetSelection(session.handle, {})

    def test_clipboard_selection_write(self):
        params = {"force-clipboard-enabled": True}
        session, _ = self.start_session(params)
        clipboard_interface = self.get_dbus_interface()

        fd_object: dbus.types.UnixFd = clipboard_interface.SelectionWrite(
            session.handle, 1234
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        bytes_written = os.write(fd, b"Clipboard")
        assert bytes_written > 0

        clipboard_interface.SelectionWriteDone(session.handle, 1234, True)

    def test_clipboard_selection_read(self):
        params = {"force-clipboard-enabled": True}
        session, _ = self.start_session(params)
        clipboard_interface = self.get_dbus_interface()

        fd_object: dbus.types.UnixFd = clipboard_interface.SelectionRead(
            session.handle, "mimetype"
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        clipboard = os.read(fd, 1000)
        assert str(clipboard)
