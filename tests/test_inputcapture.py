# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from gi.repository import GLib
from . import Response, Session
from enum import IntEnum
from itertools import count

import dbus
import pytest
import socket

counter = count()


def default_zones():
    return [(1024, 768, 0, 0), (640, 480, 1024, 0)]


@pytest.fixture
def portal_name():
    return "InputCapture"


@pytest.fixture
def zones():
    return default_zones()


class PersistMode(IntEnum):
    NONE = 0
    TRANSIENT = 1
    PERSISTENT = 2


class TestInputCapture:
    def create_session(
        self,
        portal_mock,
        capabilities=0xF,
        restore_token=None,
        persist_mode=PersistMode.NONE,
    ) -> Response:
        """
        Call CreateSession for the given capabilities and return the
        (response, results) tuple.
        """
        request = portal_mock.create_request()

        capabilities = dbus.UInt32(capabilities, variant_level=1)
        session_handle_token = dbus.String(f"session{next(counter)}", variant_level=1)

        options = {
            "capabilities": capabilities,
            "session_handle_token": session_handle_token,
        }
        if restore_token:
            options["restore_token"] = dbus.String(restore_token)

        if persist_mode:
            options["persist_mode"] = dbus.UInt32(persist_mode)

        options = dbus.Dictionary(options, signature="sv")

        response = request.call("CreateSession", parent_window="", options=options)
        assert response.response == 0
        assert "session_handle" in response.results
        assert "capabilities" in response.results
        caps = response.results["capabilities"]
        # Returned capabilities must be a subset of the requested ones
        assert caps & ~capabilities == 0

        self.current_session_handle = response.results["session_handle"]

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == capabilities
        if persist_mode:
            assert args[4]["persist_mode"] == persist_mode

        # if restore_token:
        #     # The portal converts from token to data, we don't know the exact data
        #     assert args[4]["restore_data"] is not None

        return response

    def get_zones(self, portal_mock):
        """
        Call GetZones and return the (response, results) tuple.
        """
        request = portal_mock.create_request()
        options = {}
        response, results = request.call(
            "GetZones", session_handle=self.current_session_handle, options=options
        )
        assert response == 0
        assert "zones" in results
        assert "zone_set" in results

        self.current_zone_set = results["zone_set"]

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("GetZones")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == request.handle
        assert args[1] == self.current_session_handle

        return response, results

    def set_pointer_barriers(self, portal_mock, barriers):
        request = portal_mock.create_request()
        options = {}
        response, results = request.call(
            "SetPointerBarriers",
            session_handle=self.current_session_handle,
            options=options,
            barriers=barriers,
            zone_set=self.current_zone_set,
        )
        assert response == 0
        assert "failed_barriers" in results

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("SetPointerBarriers")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == request.handle
        assert args[1] == self.current_session_handle
        assert args[4] == barriers
        assert args[5] == self.current_zone_set

        return response, results

    def connect_to_eis(self, portal_mock):
        inputcapture_intf = portal_mock.get_dbus_interface()
        fd = inputcapture_intf.ConnectToEIS(
            self.current_session_handle, dbus.Dictionary({}, signature="sv")
        )

        # Our dbusmock template sends HELLO
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        hello = eis_socket.recv(10)
        assert hello == b"HELLO"

        method_calls = portal_mock.mock_interface.GetMethodCalls("ConnectToEIS")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle

        return eis_socket

    def enable(self, portal_mock):
        inputcapture_intf = portal_mock.get_dbus_interface()
        inputcapture_intf.Enable(
            self.current_session_handle, dbus.Dictionary({}, signature="sv")
        )

        method_calls = portal_mock.mock_interface.GetMethodCalls("Enable")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle

    def disable(self, portal_mock):
        inputcapture_intf = portal_mock.get_dbus_interface()
        inputcapture_intf.Disable(
            self.current_session_handle, dbus.Dictionary({}, signature="sv")
        )

        method_calls = portal_mock.mock_interface.GetMethodCalls("Disable")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle

    def release(self, portal_mock, activation_id: int, cursor_position=None):
        options = {"activation_id": dbus.UInt32(activation_id)}
        if cursor_position:
            options["cursor_position"] = dbus.Struct(
                list(cursor_position), signature="dd", variant_level=1
            )

        inputcapture_intf = portal_mock.get_dbus_interface()
        inputcapture_intf.Release(
            self.current_session_handle, dbus.Dictionary(options, signature="sv")
        )

        method_calls = portal_mock.mock_interface.GetMethodCalls("Release")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle
        assert "activation_id" in args[2]
        aid = args[2]["activation_id"]
        assert aid == activation_id
        if cursor_position:
            assert "cursor_position" in args[2]
            pos = args[2]["cursor_position"]
            assert pos == cursor_position

    def test_version(self, portal_mock):
        portal_mock.check_version(2)

    @pytest.mark.parametrize(
        "params",
        (
            {
                "supported_capabilities": 0b101,  # KEYBOARD, POINTER, TOUCH
            },
        ),
    )
    def test_supported_capabilities(self, portal_mock):
        properties_intf = portal_mock.get_dbus_interface(
            "org.freedesktop.DBus.Properties"
        )
        caps = properties_intf.Get(
            "org.freedesktop.portal.InputCapture", "SupportedCapabilities"
        )
        assert caps == 0b101

    def test_create_session(self, portal_mock):
        self.create_session(portal_mock, capabilities=0b1)  # KEYBOARD

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == 0b1

    @pytest.mark.parametrize(
        "params",
        (
            {
                "capabilities": 0b110,  # TOUCH, POINTER
                "supported_capabilities": 0b111,  # TOUCH, POINTER, KEYBOARD
            },
        ),
    )
    def test_create_session_limited_caps(self, portal_mock):
        # Request more caps than are supported
        response, results = self.create_session(portal_mock, capabilities=0b111)
        caps = results["capabilities"]
        # Returned capabilities must the ones we set up in the params
        assert caps == 0b110

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == 0b111

    @pytest.mark.parametrize(
        "params",
        (
            {
                "default-zone": dbus.Array(
                    [dbus.Struct(z, signature="uuii") for z in default_zones()],
                    signature="(uuii)",
                    variant_level=1,
                )
            },
        ),
    )
    def test_get_zones(self, portal_mock, zones):
        response, results = self.create_session(portal_mock)
        response, results = self.get_zones(portal_mock)
        for z1, z2 in zip(results["zones"], zones):
            assert z1 == z2

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        method_calls = portal_mock.mock_interface.GetMethodCalls("GetZones")
        assert len(method_calls) == 1

    @pytest.mark.parametrize(
        "params",
        (
            {
                "default-zone": dbus.Array(
                    [dbus.Struct(z, signature="uuii") for z in default_zones()],
                    signature="(uuii)",
                    variant_level=1,
                )
            },
        ),
    )
    def test_set_pointer_barriers(self, portal_mock, zones):
        response, results = self.create_session(portal_mock)
        response, results = self.get_zones(portal_mock)

        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 0, 768], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(11, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1024, 0], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(12, variant_level=1),
                "position": dbus.Struct(
                    [1024, 0, 1024, 768], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(13, variant_level=1),
                "position": dbus.Struct(
                    [0, 768, 1024, 768], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(14, variant_level=1),
                "position": dbus.Struct(
                    [100, 768, 500, 768], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(15, variant_level=1),
                "position": dbus.Struct(
                    [1024, 0, 1024, 480], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(16, variant_level=1),
                "position": dbus.Struct(
                    [1024 + 640, 0, 1024 + 640, 480], signature="iiii", variant_level=1
                ),
            },
            # invalid ones
            {
                "barrier_id": dbus.UInt32(20, variant_level=1),
                "position": dbus.Struct(
                    [0, 1, 3, 4], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(21, variant_level=1),
                "position": dbus.Struct(
                    [0, 1, 1024, 1], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(22, variant_level=1),
                "position": dbus.Struct(
                    [1, 0, 1, 768], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(23, variant_level=1),
                "position": dbus.Struct(
                    [1023, 0, 1023, 768], signature="iiii", variant_level=1
                ),
            },
            {
                "barrier_id": dbus.UInt32(24, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1050, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        response, results = self.set_pointer_barriers(portal_mock, barriers=barriers)
        failed_barriers = results["failed_barriers"]
        assert all([id >= 20 for id in failed_barriers])

        for id in [b["barrier_id"] for b in barriers if b["barrier_id"] >= 20]:
            assert id in failed_barriers

        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        method_calls = portal_mock.mock_interface.GetMethodCalls("GetZones")
        assert len(method_calls) == 1
        method_calls = portal_mock.mock_interface.GetMethodCalls("SetPointerBarriers")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[4] == barriers
        assert args[5] == self.current_zone_set

    def test_connect_to_eis(self, portal_mock):
        self.create_session(portal_mock)
        self.get_zones(portal_mock)

        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(portal_mock, barriers)

        self.connect_to_eis(portal_mock)

    def test_enable_disable(self, portal_mock):
        self.create_session(portal_mock)
        self.create_session(portal_mock)
        self.get_zones(portal_mock)

        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(portal_mock, barriers)
        self.connect_to_eis(
            portal_mock,
        )

        # Disable before enable should be a noop
        self.disable(
            portal_mock,
        )
        method_calls = portal_mock.mock_interface.GetMethodCalls("Disable")
        assert len(method_calls) == 1

        self.enable(
            portal_mock,
        )
        method_calls = portal_mock.mock_interface.GetMethodCalls("Enable")
        assert len(method_calls) == 1

        self.disable(
            portal_mock,
        )
        method_calls = portal_mock.mock_interface.GetMethodCalls("Disable")
        assert len(method_calls) == 2

    @pytest.mark.parametrize(
        "params",
        (
            {
                "disable-delay": 200,
            },
        ),
    )
    def test_disable_signal(self, portal_mock):
        self.create_session(portal_mock)
        self.get_zones(portal_mock)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(portal_mock, barriers)
        self.connect_to_eis(portal_mock)

        disabled_signal_received = False

        def cb_disabled(session_handle, options):
            nonlocal disabled_signal_received
            disabled_signal_received = True
            assert session_handle == session_handle

        inputcapture_intf = portal_mock.get_dbus_interface()
        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)

        self.enable(portal_mock)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()

        assert disabled_signal_received

    @pytest.mark.parametrize(
        "params",
        (
            {
                "activated-delay": 200,
                "deactivated-delay": 300,
            },
        ),
    )
    def test_activated_signal(self, portal_mock):
        self.create_session(portal_mock)
        self.get_zones(portal_mock)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(portal_mock, barriers)
        self.connect_to_eis(portal_mock)

        disabled_signal_received = False
        activated_signal_received = False
        deactivated_signal_received = False

        def cb_disabled(session_handle, options):
            nonlocal disabled_signal_received
            disabled_signal_received = True

        def cb_activated(session_handle, options):
            nonlocal activated_signal_received
            activated_signal_received = True
            assert session_handle == session_handle
            assert "activation_id" in options
            assert "barrier_id" in options
            assert options["barrier_id"] == 10  # template uses first barrier
            assert "cursor_position" in options
            assert options["cursor_position"] == (
                10.0,
                20.0,
            )  # template uses x+10, y+20 of first barrier

        def cb_deactivated(session_handle, options):
            nonlocal deactivated_signal_received
            deactivated_signal_received = True
            assert session_handle == session_handle
            assert "activation_id" in options
            assert "cursor_position" in options
            assert options["cursor_position"] == (
                10.0,
                20.0,
            )  # template uses x+10, y+20 of first barrier

        inputcapture_intf = portal_mock.get_dbus_interface()
        inputcapture_intf.connect_to_signal("Activated", cb_activated)
        inputcapture_intf.connect_to_signal("Deactivated", cb_deactivated)
        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)

        self.enable(portal_mock)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()

        assert activated_signal_received
        assert deactivated_signal_received
        assert not disabled_signal_received

        # Disabling should not trigger the signal
        self.disable(portal_mock)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()

        assert not disabled_signal_received

    @pytest.mark.parametrize(
        "params",
        (
            {
                "activated-delay": 200,
                "deactivated-delay": 1000,
                "disabled-delay": 1200,
            },
        ),
    )
    def test_release(self, portal_mock):
        self.create_session(portal_mock)
        self.get_zones(portal_mock)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(portal_mock, barriers)
        self.connect_to_eis(portal_mock)

        disabled_signal_received = False
        activated_signal_received = False
        deactivated_signal_received = False
        activation_id = None

        def cb_disabled(session_handle, options):
            nonlocal disabled_signal_received
            disabled_signal_received = True

        def cb_activated(session_handle, options):
            nonlocal activated_signal_received, activation_id
            activated_signal_received = True
            activation_id = options["activation_id"]

        def cb_deactivated(session_handle, options):
            nonlocal deactivated_signal_received
            deactivated_signal_received = True

        inputcapture_intf = portal_mock.get_dbus_interface()
        inputcapture_intf.connect_to_signal("Disabled", cb_activated)
        inputcapture_intf.connect_to_signal("Activated", cb_activated)
        inputcapture_intf.connect_to_signal("Deactivated", cb_deactivated)

        self.enable(portal_mock)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        assert activated_signal_received
        assert activation_id is not None
        assert not deactivated_signal_received
        assert not disabled_signal_received

        self.release(
            portal_mock, cursor_position=(10.0, 50.0), activation_id=activation_id
        )

        # XDP should filter any signals the implementation may
        # send after Release().

        mainloop = GLib.MainLoop()
        GLib.timeout_add(1000, mainloop.quit)
        mainloop.run()

        # Release() implies deactivated
        assert not deactivated_signal_received
        assert not disabled_signal_received

    def test_restore_session(self, portal_mock):
        import uuid

        # This should initialize a session with restore_data
        response = self.create_session(portal_mock, persist_mode=PersistMode.TRANSIENT)
        assert response.response == 0
        restore_token = response.results["restore_token"]
        assert restore_token is not None
        try:
            # We cannot see the actual data but we can verify our token is a valid UUID
            uuid.UUID(restore_token)
        except ValueError as e:
            pytest.fail(f"Invalid UUID: {e}")

        session = Session.from_response(portal_mock.dbus_con, response)
        session.close()

        mainloop = GLib.MainLoop()
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()

        assert session.closed

        # Second session, try to restore it with the token
        response, results = self.create_session(
            portal_mock, persist_mode=PersistMode.TRANSIENT, restore_token=restore_token
        )
        assert response == 0
        assert results["restore_token"] is not None
        # An implementation detail, the spec does not require it
        assert results["restore_token"] == restore_token

        # Third session, try to restore with an invalid token which is silently ignored
        # but should give us a new restore token
        response, results = self.create_session(
            portal_mock,
            persist_mode=PersistMode.TRANSIENT,
            restore_token=str(uuid.uuid4()),
        )
        assert response == 0
        assert results["restore_token"] is not None
        assert results["restore_token"] != restore_token

        # Fourth session, try to restore with a non-uuid value
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            self.create_session(
                portal_mock, persist_mode=PersistMode.TRANSIENT, restore_token="blah"
            )
            assert (
                "Restore token is not a valid UUID string"
                in excinfo.value.get_dbus_message()
            )
