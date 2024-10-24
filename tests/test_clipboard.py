# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests import Session
import dbus
import pytest
import os


@pytest.fixture
def portal_name():
    return "Clipboard"


@pytest.fixture
def required_templates():
    return {
        "Clipboard": {},
        "RemoteDesktop": {"force-clipboard-enabled": True},
    }


class TestClipboard:
    def test_version(self, portal_mock):
        portal_mock.check_version(1)

    def start_session(self, pmock):
        create_session_request = pmock.create_request("RemoteDesktop")
        create_session_response = create_session_request.call(
            "CreateSession", options={"session_handle_token": "1234"}
        )
        assert create_session_response.response == 0
        assert str(create_session_response.results["session_handle"])

        session = Session.from_response(pmock.dbus_con, create_session_response)

        clipboard_interface = pmock.get_dbus_interface()
        clipboard_interface.RequestClipboard(session.handle, {})

        start_session_request = pmock.create_request("RemoteDesktop")
        start_session_response = start_session_request.call(
            "Start", session_handle=session.handle, parent_window="", options={}
        )

        assert start_session_response.response == 0

        return (session, start_session_response.results.get("clipboard_enabled"))

    def test_request_clipboard_and_start_session(self, portal_mock):
        _, clipboard_enabled = self.start_session(portal_mock)

        assert clipboard_enabled

    @pytest.mark.parametrize(
        "template_params", ({"RemoteDesktop": {"force-clipboard-enabled": False}},)
    )
    def test_clipboard_checks_clipboard_enabled(self, portal_mock):
        session, clipboard_enabled = self.start_session(portal_mock)
        clipboard_interface = portal_mock.get_dbus_interface()

        assert not clipboard_enabled

        with pytest.raises(dbus.exceptions.DBusException):
            clipboard_interface.SetSelection(session.handle, {})

    def test_clipboard_set_selection(self, portal_mock):
        session, _ = self.start_session(portal_mock)
        clipboard_interface = portal_mock.get_dbus_interface()

        clipboard_interface.SetSelection(session.handle, {})

    def test_clipboard_selection_write(self, portal_mock):
        session, _ = self.start_session(portal_mock)
        clipboard_interface = portal_mock.get_dbus_interface()

        fd_object: dbus.types.UnixFd = clipboard_interface.SelectionWrite(
            session.handle, 1234
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        bytes_written = os.write(fd, b"Clipboard")
        assert bytes_written > 0

        clipboard_interface.SelectionWriteDone(session.handle, 1234, True)

    def test_clipboard_selection_read(self, portal_mock):
        session, _ = self.start_session(portal_mock)
        clipboard_interface = portal_mock.get_dbus_interface()

        fd_object: dbus.types.UnixFd = clipboard_interface.SelectionRead(
            session.handle, "mimetype"
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        clipboard = os.read(fd, 1000)
        assert str(clipboard)
