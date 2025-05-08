# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
import socket
from gi.repository import GLib
from itertools import count
from typing import Any


counter = count()


def default_zones():
    return [(1024, 768, 0, 0), (640, 480, 1024, 0)]


@pytest.fixture
def required_templates():
    return {"inputcapture": {}}


@pytest.fixture
def zones():
    return default_zones()


class TestInputCapture:
    def create_session(self, dbus_con, capabilities=0xF):
        """
        Call CreateSession for the given capabilities and return the
        (response, results) tuple.
        """
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, inputcapture_intf)

        capabilities = dbus.UInt32(capabilities, variant_level=1)
        session_handle_token = dbus.String(f"session{next(counter)}", variant_level=1)

        options = dbus.Dictionary(
            {
                "capabilities": capabilities,
                "session_handle_token": session_handle_token,
            },
            signature="sv",
        )

        response = request.call("CreateSession", parent_window="", options=options)

        assert response
        assert response.response == 0
        assert "session_handle" in response.results
        assert "capabilities" in response.results
        caps = response.results["capabilities"]
        # Returned capabilities must be a subset of the requested ones
        assert caps & ~capabilities == 0

        self.current_session_handle = response.results["session_handle"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == capabilities

        return response

    def get_zones(self, dbus_con):
        """
        Call GetZones and return the (response, results) tuple.
        """
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, inputcapture_intf)
        options: Any = {}
        response = request.call(
            "GetZones", session_handle=self.current_session_handle, options=options
        )

        assert response
        assert response.response == 0
        assert "zones" in response.results
        assert "zone_set" in response.results

        self.current_zone_set = response.results["zone_set"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("GetZones")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == request.handle
        assert args[1] == self.current_session_handle

        return response

    def set_pointer_barriers(self, dbus_con, barriers):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, inputcapture_intf)
        options: Any = {}
        response = request.call(
            "SetPointerBarriers",
            session_handle=self.current_session_handle,
            options=options,
            barriers=barriers,
            zone_set=self.current_zone_set,
        )
        assert response
        assert response.response == 0
        assert "failed_barriers" in response.results

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SetPointerBarriers")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == request.handle
        assert args[1] == self.current_session_handle
        assert args[4] == barriers
        assert args[5] == self.current_zone_set

        return response

    def connect_to_eis(self, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd = inputcapture_intf.ConnectToEIS(
            self.current_session_handle, dbus.Dictionary({}, signature="sv")
        )

        # Our dbusmock template sends HELLO
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        hello = eis_socket.recv(10)
        assert hello == b"HELLO"

        method_calls = mock_intf.GetMethodCalls("ConnectToEIS")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle

        return eis_socket

    def enable(self, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        inputcapture_intf.Enable(
            self.current_session_handle, dbus.Dictionary({}, signature="sv")
        )

        method_calls = mock_intf.GetMethodCalls("Enable")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle

    def disable(self, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        inputcapture_intf.Disable(
            self.current_session_handle, dbus.Dictionary({}, signature="sv")
        )

        method_calls = mock_intf.GetMethodCalls("Disable")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.current_session_handle

    def release(self, dbus_con, activation_id: int, cursor_position=None):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(dbus_con)

        options = {"activation_id": dbus.UInt32(activation_id)}
        if cursor_position:
            options["cursor_position"] = dbus.Struct(
                list(cursor_position), signature="dd", variant_level=1
            )

        inputcapture_intf.Release(
            self.current_session_handle, dbus.Dictionary(options, signature="sv")
        )

        method_calls = mock_intf.GetMethodCalls("Release")
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

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "InputCapture", 1)

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "supported_capabilities": 0b101,  # KEYBOARD, POINTER, TOUCH
                },
            },
        ),
    )
    def test_supported_capabilities(self, portals, dbus_con):
        properties_intf = xdp.get_iface(dbus_con, "org.freedesktop.DBus.Properties")

        caps = properties_intf.Get(
            "org.freedesktop.portal.InputCapture", "SupportedCapabilities"
        )
        assert caps == 0b101

    def test_create_session(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con)

        self.create_session(dbus_con, capabilities=0b1)  # KEYBOARD

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == 0b1

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "capabilities": 0b110,  # TOUCH, POINTER
                    "supported_capabilities": 0b111,  # TOUCH, POINTER, KEYBOARD
                },
            },
        ),
    )
    def test_create_session_limited_caps(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con)

        # Request more caps than are supported
        response, results = self.create_session(dbus_con, capabilities=0b111)
        caps = results["capabilities"]
        # Returned capabilities must the ones we set up in the params
        assert caps == 0b110

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == 0b111

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "default-zone": dbus.Array(
                        [dbus.Struct(z, signature="uuii") for z in default_zones()],
                        signature="(uuii)",
                        variant_level=1,
                    )
                },
            },
        ),
    )
    def test_get_zones(self, portals, dbus_con, zones):
        mock_intf = xdp.get_mock_iface(dbus_con)

        response, results = self.create_session(dbus_con)
        response, results = self.get_zones(dbus_con)
        for z1, z2 in zip(results["zones"], zones):
            assert z1 == z2

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        method_calls = mock_intf.GetMethodCalls("GetZones")
        assert len(method_calls) == 1

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "default-zone": dbus.Array(
                        [dbus.Struct(z, signature="uuii") for z in default_zones()],
                        signature="(uuii)",
                        variant_level=1,
                    )
                },
            },
        ),
    )
    def test_set_pointer_barriers(self, portals, dbus_con, zones):
        mock_intf = xdp.get_mock_iface(dbus_con)

        response, results = self.create_session(dbus_con)
        response, results = self.get_zones(dbus_con)

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
        response, results = self.set_pointer_barriers(dbus_con, barriers=barriers)
        failed_barriers = results["failed_barriers"]
        assert all([id >= 20 for id in failed_barriers])

        for id in [b["barrier_id"] for b in barriers if b["barrier_id"] >= 20]:
            assert id in failed_barriers

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) == 1
        method_calls = mock_intf.GetMethodCalls("GetZones")
        assert len(method_calls) == 1
        method_calls = mock_intf.GetMethodCalls("SetPointerBarriers")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[4] == barriers
        assert args[5] == self.current_zone_set

    def test_connect_to_eis(self, portals, dbus_con):
        self.create_session(dbus_con)
        self.get_zones(dbus_con)

        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(dbus_con, barriers)

        self.connect_to_eis(dbus_con)

    def test_enable_disable(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con)

        self.create_session(dbus_con)
        self.get_zones(dbus_con)

        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(dbus_con, barriers)
        self.connect_to_eis(dbus_con)

        # Disable before enable should be a noop
        self.disable(dbus_con)

        method_calls = mock_intf.GetMethodCalls("Disable")
        assert len(method_calls) == 1

        self.enable(dbus_con)
        method_calls = mock_intf.GetMethodCalls("Enable")
        assert len(method_calls) == 1

        self.disable(dbus_con)
        method_calls = mock_intf.GetMethodCalls("Disable")
        assert len(method_calls) == 2

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "disable-delay": 200,
                },
            },
        ),
    )
    def test_disable_signal(self, portals, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")

        self.create_session(dbus_con)
        self.get_zones(dbus_con)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(dbus_con, barriers)
        self.connect_to_eis(dbus_con)

        disabled_signal_received = False

        def cb_disabled(session_handle, options):
            nonlocal disabled_signal_received
            disabled_signal_received = True
            assert session_handle == session_handle

        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)
        self.enable(dbus_con)
        xdp.wait_for(lambda: disabled_signal_received)

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "activated-delay": 200,
                    "deactivated-delay": 300,
                },
            },
        ),
    )
    def test_activated_signal(self, portals, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")

        self.create_session(dbus_con)
        self.get_zones(dbus_con)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(dbus_con, barriers)
        self.connect_to_eis(dbus_con)

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

        inputcapture_intf.connect_to_signal("Activated", cb_activated)
        inputcapture_intf.connect_to_signal("Deactivated", cb_deactivated)
        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)

        self.enable(dbus_con)

        xdp.wait_for(lambda: activated_signal_received and deactivated_signal_received)
        assert not disabled_signal_received

        # Disabling should not trigger the signal
        self.disable(dbus_con)
        assert not disabled_signal_received

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "zones-changed-delay": 200,
                },
            },
        ),
    )
    def test_zones_changed_signal(self, portals, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")

        self.create_session(dbus_con)
        self.get_zones(dbus_con)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(dbus_con, barriers)
        self.connect_to_eis(dbus_con)

        zones_changed_signal_received = False

        def cb_zones_changed(session_handle, options):
            nonlocal zones_changed_signal_received
            zones_changed_signal_received = True
            assert session_handle == session_handle

        inputcapture_intf.connect_to_signal("ZonesChanged", cb_zones_changed)
        self.enable(dbus_con)
        xdp.wait_for(lambda: zones_changed_signal_received)

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "inputcapture": {
                    "activated-delay": 200,
                    "deactivated-delay": 1000,
                    "disabled-delay": 1200,
                },
            },
        ),
    )
    def test_release(self, portals, dbus_con):
        inputcapture_intf = xdp.get_portal_iface(dbus_con, "InputCapture")

        self.create_session(dbus_con)
        self.get_zones(dbus_con)
        # The default zone is 1920x1080
        barriers = [
            {
                "barrier_id": dbus.UInt32(10, variant_level=1),
                "position": dbus.Struct(
                    [0, 0, 1920, 0], signature="iiii", variant_level=1
                ),
            },
        ]
        self.set_pointer_barriers(dbus_con, barriers)
        self.connect_to_eis(dbus_con)

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

        inputcapture_intf.connect_to_signal("Disabled", cb_activated)
        inputcapture_intf.connect_to_signal("Activated", cb_activated)
        inputcapture_intf.connect_to_signal("Deactivated", cb_deactivated)

        self.enable(dbus_con)

        xdp.wait_for(lambda: activated_signal_received)
        assert activation_id is not None
        assert not deactivated_signal_received
        assert not disabled_signal_received

        self.release(
            dbus_con, cursor_position=(10.0, 50.0), activation_id=activation_id
        )

        # XDP should filter any signals the implementation may
        # send after Release().

        mainloop = GLib.MainLoop()
        GLib.timeout_add(1000, mainloop.quit)
        mainloop.run()

        # Release() implies deactivated
        assert not deactivated_signal_received
        assert not disabled_signal_received
