# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp
import tests.test_inputcapture as inputcapture

import dbus
import pytest
import os
from enum import Enum


@pytest.fixture
def required_templates():
    return {
        "Clipboard": {},
        "RemoteDesktop": {"force-clipboard-enabled": True},
        "InputCapture": {
            "force-clipboard-enabled": True,
            "activated-delay": 100,
        },
    }


class SessionType(Enum):
    REMOTE_DESKTOP = 1
    INPUT_CAPTURE = 2


@pytest.mark.parametrize(
    "type", (SessionType.REMOTE_DESKTOP, SessionType.INPUT_CAPTURE)
)
class TestClipboard:
    def test_version(self, portals, dbus_con, type):
        xdp.check_version(dbus_con, "Clipboard", 1)

    def start_remote_desktop_session(self, dbus_con):
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

    def start_input_capture_session(self, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")

        session = inputcapture.InputcaptureSession(dbus_con)
        session.create()
        start_session_response = session.start()
        session.get_zones()
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        session.set_pointer_barriers(barriers)
        session.connect_to_eis()

        activated_signal_received = False

        def cb_activated(session_handle, options):
            nonlocal activated_signal_received
            activated_signal_received = True

        inputcapture_intf.connect_to_signal("Activated", cb_activated)

        session.enable()

        xdp.wait_for(lambda: activated_signal_received)

        return (
            session.session,
            start_session_response.results.get("clipboard_enabled"),
        )

    def start_session(self, dbus_con, type):
        if type == SessionType.REMOTE_DESKTOP:
            return self.start_remote_desktop_session(dbus_con)
        if type == SessionType.INPUT_CAPTURE:
            return self.start_input_capture_session(dbus_con)
        raise Exception("Unknown type")

    def test_request_clipboard_and_start_session(self, portals, dbus_con, type):
        _, clipboard_enabled = self.start_session(dbus_con, type)

        assert clipboard_enabled

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "RemoteDesktop": {"force-clipboard-enabled": False},
                "InputCapture": {
                    "force-clipboard-enabled": False,
                    "activated-delay": 100,
                },
            },
        ),
    )
    def test_checks_clipboard_enabled(self, portals, dbus_con, type):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, clipboard_enabled = self.start_session(dbus_con, type)

        assert not clipboard_enabled

        with pytest.raises(dbus.exceptions.DBusException):
            clipboard_intf.SetSelection(session.handle, {})

    def test_set_selection(self, portals, dbus_con, type):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, _ = self.start_session(dbus_con, type)

        clipboard_intf.SetSelection(session.handle, {})

    def test_selection_write(self, portals, dbus_con, type):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, _ = self.start_session(dbus_con, type)

        fd_object: dbus.types.UnixFd = clipboard_intf.SelectionWrite(
            session.handle, 1234
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        bytes_written = os.write(fd, b"Clipboard")
        assert bytes_written > 0

        clipboard_intf.SelectionWriteDone(session.handle, 1234, True)

    def test_selection_read(self, portals, dbus_con, type):
        clipboard_intf = xdp.get_portal_iface(dbus_con, "Clipboard")
        session, _ = self.start_session(dbus_con, type)

        fd_object: dbus.types.UnixFd = clipboard_intf.SelectionRead(
            session.handle, "mimetype"
        )
        assert fd_object

        fd = fd_object.take()
        assert fd

        clipboard_contents = os.read(fd, 1000)
        assert str(clipboard_contents)
