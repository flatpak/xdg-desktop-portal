# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import socket
from itertools import count
from typing import Any

import dbus
import pytest
from gi.repository import GLib

import tests.xdp_utils as xdp
from tests.test_varlink import VarlinkConnection

counter = count()


def default_zones():
    return [(1024, 768, 0, 0), (640, 480, 1024, 0)]


@pytest.fixture
def required_templates():
    return {"inputcapture": {}}


@pytest.fixture
def zones():
    return default_zones()


class InputcaptureSession:
    def __init__(self, dbus_con):
        self.dbus_con = dbus_con
        self.type = None
        self.session = None
        self.session_handle = None
        self.current_zone_set = []

    def create_legacy(self, capabilities=0x7):
        """
        Call CreateSession for the given capabilities and return the
        (response, results) tuple.
        """

        assert self.type is None
        self.type = "legacy"

        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        request = xdp.Request(self.dbus_con, inputcapture_intf)

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

        self.session = xdp.Session(self.dbus_con, response.results["session_handle"])
        self.session_handle = self.session.handle

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Start")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[3] == ""  # parent window
        assert args[4]["capabilities"] == capabilities

        return response

    def create(self):
        """
        Call CreateSession for the given capabilities and return the
        (response, results) tuple.
        """

        assert self.type is None
        self.type = "modern"

        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")

        session_handle_token = dbus.String(f"session{next(counter)}", variant_level=1)

        result = inputcapture_intf.CreateSession2(
            {"session_handle_token": session_handle_token}
        )
        self.session = xdp.Session(self.dbus_con, result["session_handle"])
        self.session_handle = self.session.handle

    def start(
        self,
        capabilities=0x7,
        persist_mode: xdp.SessionPersistenceMode | None = None,
        restore_token: str | None = None,
        validate_token_data: bool = True,
    ):
        assert self.type == "modern"

        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        capabilities = dbus.UInt32(capabilities, variant_level=1)
        request = xdp.Request(self.dbus_con, inputcapture_intf)
        options = {
            "capabilities": capabilities,
        }
        if persist_mode is not None:
            options["persist_mode"] = dbus.UInt32(persist_mode, variant_level=1)
        if restore_token is not None:
            options["restore_token"] = restore_token
        response = request.call(
            "Start",
            session_handle=self.session_handle,
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert "capabilities" in response.results
        caps = response.results["capabilities"]
        # Returned capabilities must be a subset of the requested ones
        assert caps & ~capabilities == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Start")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[3] == ""  # parent window
        options = args[4]
        assert options["capabilities"] == capabilities
        if persist_mode is not None:
            assert options["persist_mode"] == dbus.UInt32(persist_mode, variant_level=1)
        if restore_token is not None and validate_token_data:
            restore_data = options["restore_data"]
            assert restore_data is not None
            # Vendor-name and version are hardcoded in the template
            assert restore_data[0] == "GNOME"
            assert restore_data[1] == dbus.UInt32(7)

        return response

    def get_zones(self):
        """
        Call GetZones and return the (response, results) tuple.
        """
        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        request = xdp.Request(self.dbus_con, inputcapture_intf)
        options: Any = {}
        response = request.call(
            "GetZones", session_handle=self.session_handle, options=options
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
        assert args[1] == self.session_handle

        return response

    def set_pointer_barriers(self, barriers):
        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        request = xdp.Request(self.dbus_con, inputcapture_intf)
        options: Any = {}
        response = request.call(
            "SetPointerBarriers",
            session_handle=self.session_handle,
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
        assert args[1] == self.session_handle
        assert args[4] == barriers
        assert args[5] == self.current_zone_set

        return response

    def connect_to_eis(self):
        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        fd = inputcapture_intf.ConnectToEIS(
            self.session_handle, dbus.Dictionary({}, signature="sv")
        )

        # Our dbusmock template sends HELLO
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        hello = eis_socket.recv(10)
        assert hello == b"HELLO"

        method_calls = mock_intf.GetMethodCalls("ConnectToEIS")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.session_handle

        return eis_socket

    def enable(self):
        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        inputcapture_intf.Enable(
            self.session_handle, dbus.Dictionary({}, signature="sv")
        )

        method_calls = mock_intf.GetMethodCalls("Enable")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.session_handle

    def disable(self):
        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        inputcapture_intf.Disable(
            self.session_handle, dbus.Dictionary({}, signature="sv")
        )

        method_calls = mock_intf.GetMethodCalls("Disable")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.session_handle

    def release(self, activation_id: int, cursor_position=None):
        inputcapture_intf = xdp.get_portal_iface(self.dbus_con, "InputCapture")
        mock_intf = xdp.get_mock_iface(self.dbus_con)

        options = {"activation_id": dbus.UInt32(activation_id)}
        if cursor_position:
            options["cursor_position"] = dbus.Struct(
                list(cursor_position), signature="dd", variant_level=1
            )

        inputcapture_intf.Release(
            self.session_handle, dbus.Dictionary(options, signature="sv")
        )

        method_calls = mock_intf.GetMethodCalls("Release")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == self.session_handle
        assert "activation_id" in args[2]
        aid = args[2]["activation_id"]
        assert aid == activation_id
        if cursor_position:
            assert "cursor_position" in args[2]
            pos = args[2]["cursor_position"]
            assert pos == cursor_position


class TestInputCapture:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "InputCapture", 2)

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
        session = InputcaptureSession(dbus_con)
        session.create()  # KEYBOARD
        session.start(capabilities=0b1)  # KEYBOARD
        session = InputcaptureSession(dbus_con)
        session.create_legacy(capabilities=0b1)  # KEYBOARD

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
        # Request more caps than are supported
        session = InputcaptureSession(dbus_con)
        session.create()
        _, results = session.start(capabilities=0b111)
        caps = results["capabilities"]
        # Returned capabilities must the ones we set up in the params
        assert caps == 0b110

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

        session = InputcaptureSession(dbus_con)
        session.create()
        _, results = session.start()
        _, results = session.get_zones()
        for z1, z2 in zip(results["zones"], zones):
            assert z1 == z2

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Start")
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

        session = InputcaptureSession(dbus_con)
        session.create()
        _, results = session.start()
        _, results = session.get_zones()

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
        _, results = session.set_pointer_barriers(barriers=barriers)
        failed_barriers = results["failed_barriers"]
        assert all(id >= 20 for id in failed_barriers)

        for id in [b["barrier_id"] for b in barriers if b["barrier_id"] >= 20]:
            assert id in failed_barriers

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Start")
        assert len(method_calls) == 1
        method_calls = mock_intf.GetMethodCalls("GetZones")
        assert len(method_calls) == 1
        method_calls = mock_intf.GetMethodCalls("SetPointerBarriers")
        assert len(method_calls) == 1
        _, args = method_calls.pop(0)
        assert args[4] == barriers
        assert args[5] == session.current_zone_set

    def test_connect_to_eis(self, portals, dbus_con):
        session = InputcaptureSession(dbus_con)
        session.create()
        session.start()
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

    def test_enable_disable(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con)

        session = InputcaptureSession(dbus_con)
        session.create()
        session.start()
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

        # Disable before enable should be a noop
        session.disable()

        method_calls = mock_intf.GetMethodCalls("Disable")
        assert len(method_calls) == 1

        session.enable()
        method_calls = mock_intf.GetMethodCalls("Enable")
        assert len(method_calls) == 1

        session.disable()
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

        session = InputcaptureSession(dbus_con)
        session.create()
        session.start()
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

        disabled_signal_received = False

        def cb_disabled(session_handle, options):
            nonlocal disabled_signal_received
            disabled_signal_received = True

        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)
        session.enable()
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

        session = InputcaptureSession(dbus_con)
        session.create()
        session.start()
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

        disabled_signal_received = False
        activated_signal_received = False
        deactivated_signal_received = False

        def cb_disabled(session_handle, options):
            nonlocal disabled_signal_received
            disabled_signal_received = True

        def cb_activated(session_handle, options):
            nonlocal activated_signal_received
            activated_signal_received = True
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
            assert "activation_id" in options
            assert "cursor_position" in options
            assert options["cursor_position"] == (
                10.0,
                20.0,
            )  # template uses x+10, y+20 of first barrier

        inputcapture_intf.connect_to_signal("Activated", cb_activated)
        inputcapture_intf.connect_to_signal("Deactivated", cb_deactivated)
        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)

        session.enable()

        xdp.wait_for(lambda: activated_signal_received and deactivated_signal_received)
        assert not disabled_signal_received

        # Disabling should not trigger the signal
        session.disable()
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

        session = InputcaptureSession(dbus_con)
        session.create()
        session.start()
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

        zones_changed_signal_received = False

        def cb_zones_changed(session_handle, options):
            nonlocal zones_changed_signal_received
            zones_changed_signal_received = True

        inputcapture_intf.connect_to_signal("ZonesChanged", cb_zones_changed)
        session.enable()
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

        session = InputcaptureSession(dbus_con)
        session.create()
        session.start()
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

        inputcapture_intf.connect_to_signal("Disabled", cb_disabled)
        inputcapture_intf.connect_to_signal("Activated", cb_activated)
        inputcapture_intf.connect_to_signal("Deactivated", cb_deactivated)

        session.enable()

        xdp.wait_for(lambda: activated_signal_received)
        assert activation_id is not None
        assert not deactivated_signal_received
        assert not disabled_signal_received

        session.release(cursor_position=(10.0, 50.0), activation_id=activation_id)

        # XDP should filter any signals the implementation may
        # send after Release().

        mainloop = GLib.MainLoop()
        GLib.timeout_add(1000, mainloop.quit)
        mainloop.run()

        # Release() implies deactivated
        assert not deactivated_signal_received
        assert not disabled_signal_received

    @pytest.mark.parametrize(
        "mode",
        [xdp.SessionPersistenceMode.TRANSIENT, xdp.SessionPersistenceMode.PERSISTENT],
    )
    def test_persistence(self, portals, dbus_con, mode):
        def create_persistent_session(
            persist_mode: xdp.SessionPersistenceMode,
            restore_token: str | None,
            validate_token_data: bool = True,
        ):
            ic_session = InputcaptureSession(dbus_con)
            ic_session.create()
            response = ic_session.start(
                restore_token=restore_token,
                persist_mode=persist_mode,
                validate_token_data=validate_token_data,
            )
            restore_token = response.results.get("restore_token")
            session = xdp.Session.from_response(dbus_con, response)
            session.close()
            return restore_token

        token1 = create_persistent_session(persist_mode=mode, restore_token=None)
        assert token1 is not None

        # Current implementation keeps the restore token for the same data
        token2 = create_persistent_session(persist_mode=mode, restore_token=token1)
        assert token2 is not None
        assert token2 == token1

        # A new session with different token data
        token3 = create_persistent_session(persist_mode=mode, restore_token=None)
        assert token3 is not None
        assert token3 != token1

        # First session again
        token4 = create_persistent_session(persist_mode=mode, restore_token=token1)
        assert token4 is not None
        assert token4 == token1

        # End the first session
        token5 = create_persistent_session(
            persist_mode=xdp.SessionPersistenceMode.NONE, restore_token=token1
        )
        assert token5 is None

        # token1 is invalid now
        token6 = create_persistent_session(
            persist_mode=xdp.SessionPersistenceMode.PERSISTENT,
            restore_token=token1,
            validate_token_data=False,
        )
        assert token6 is not None
        assert token6 != token1

        # but token3 is still there
        token7 = create_persistent_session(persist_mode=mode, restore_token=token3)
        assert token7 is not None
        assert token7 == token3


INPUT_CAPTURE = "org.freedesktop.portal.InputCapture"


class TestVarlinkInputCapture:
    @pytest.fixture
    def xdp_app_info(self) -> xdp.AppInfo:
        return xdp.AppInfoHost()

    def create_session(self, conn):
        """
        Create a session on @conn and return its id; the call stays parked, so
        @conn cannot be used for anything else.
        """
        conn.send(f"{INPUT_CAPTURE}.CreateSession", more=True)
        reply = conn.receive()
        assert "error" not in reply, reply
        assert reply["continues"]

        return reply["parameters"]["session"]

    def start(self, conn, session, capabilities=None):
        reply = conn.call(
            f"{INPUT_CAPTURE}.Start",
            session=session,
            parent_window="",
            capabilities=capabilities or ["keyboard", "pointer"],
        )
        assert "error" not in reply, reply
        return reply["parameters"]

    def test_get_supported_capabilities(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply = conn.call(f"{INPUT_CAPTURE}.GetSupportedCapabilities")
            assert "error" not in reply, reply
            assert set(reply["parameters"]["capabilities"]) == {
                "keyboard",
                "pointer",
                "touchscreen",
            }

    def test_create_session_requires_more(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply = conn.call(f"{INPUT_CAPTURE}.CreateSession")
            assert reply["error"] == f"{INPUT_CAPTURE}.ExpectedMore"

    def test_create_session_before_registering(self, portals):
        with VarlinkConnection() as conn:
            conn.send(f"{INPUT_CAPTURE}.CreateSession", more=True)
            reply = conn.receive()
            assert reply["error"] == f"{INPUT_CAPTURE}.NotRegistered"

    def test_create_and_start(self, portals):
        with VarlinkConnection() as session_conn:
            instance = session_conn.register()
            session = self.create_session(session_conn)

            with VarlinkConnection() as conn:
                conn.call(
                    "org.freedesktop.host.portal.Registry.Claim",
                    instance_id=instance,
                )

                results = self.start(conn, session)
                assert set(results["capabilities"]) == {"keyboard", "pointer"}
                assert results["clipboard_enabled"] is False

    def test_unknown_session(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply = conn.call(f"{INPUT_CAPTURE}.GetZones", session=9999)
            assert reply["error"] == f"{INPUT_CAPTURE}.NoSuchSession"

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
    def test_zones_and_barriers(self, portals, zones):
        with VarlinkConnection() as session_conn:
            instance = session_conn.register()
            session = self.create_session(session_conn)

            with VarlinkConnection() as conn:
                conn.call(
                    "org.freedesktop.host.portal.Registry.Claim",
                    instance_id=instance,
                )
                self.start(conn, session)

                reply = conn.call(f"{INPUT_CAPTURE}.GetZones", session=session)
                assert "error" not in reply, reply

                got = [
                    (z["width"], z["height"], z["x"], z["y"])
                    for z in reply["parameters"]["zones"]
                ]
                assert got == zones

                zone_set = reply["parameters"]["zone_set"]

                reply = conn.call(
                    f"{INPUT_CAPTURE}.SetPointerBarriers",
                    session=session,
                    zone_set=zone_set,
                    barriers=[
                        {"barrier_id": 10, "x1": 0, "y1": 0, "x2": 1024, "y2": 0}
                    ],
                )
                assert "error" not in reply, reply
                assert reply["parameters"]["failed_barriers"] == []

    def test_connect_to_eis(self, portals):
        with VarlinkConnection() as session_conn:
            instance = session_conn.register()
            session = self.create_session(session_conn)

            with VarlinkConnection() as conn:
                conn.call(
                    "org.freedesktop.host.portal.Registry.Claim",
                    instance_id=instance,
                )
                self.start(conn, session)

                reply = conn.call(f"{INPUT_CAPTURE}.ConnectToEIS", session=session)
                assert "error" not in reply, reply
                assert reply["parameters"]["fd_idx"] == 0
                assert len(conn.fds) == 1

    def test_session_ends_with_its_connection(self, portals):
        session_conn = VarlinkConnection()
        instance = session_conn.register()
        session = self.create_session(session_conn)

        with VarlinkConnection() as conn:
            conn.call(
                "org.freedesktop.host.portal.Registry.Claim", instance_id=instance
            )
            self.start(conn, session)

            session_conn.close()

            xdp.wait_for(
                lambda: conn.call(
                    f"{INPUT_CAPTURE}.GetZones", session=session
                ).get("error")
                == f"{INPUT_CAPTURE}.NoSuchSession"
            )

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
    def test_subscribe_capture_status(self, portals):
        with VarlinkConnection() as session_conn:
            instance = session_conn.register()
            session = self.create_session(session_conn)

            with VarlinkConnection() as conn, VarlinkConnection() as status_conn:
                for c in (conn, status_conn):
                    c.call(
                        "org.freedesktop.host.portal.Registry.Claim",
                        instance_id=instance,
                    )

                self.start(conn, session)

                reply = conn.call(f"{INPUT_CAPTURE}.ConnectToEIS", session=session)
                assert "error" not in reply, reply

                # The default zone is 1920x1080
                reply = conn.call(f"{INPUT_CAPTURE}.GetZones", session=session)
                zone_set = reply["parameters"]["zone_set"]
                conn.call(
                    f"{INPUT_CAPTURE}.SetPointerBarriers",
                    session=session,
                    zone_set=zone_set,
                    barriers=[
                        {"barrier_id": 10, "x1": 0, "y1": 0, "x2": 1920, "y2": 0}
                    ],
                )

                status_conn.send(
                    f"{INPUT_CAPTURE}.SubscribeCaptureStatus",
                    more=True,
                    session=session,
                )

                reply = conn.call(f"{INPUT_CAPTURE}.Enable", session=session)
                assert "error" not in reply, reply

                reply = status_conn.receive()
                assert "error" not in reply, reply
                assert reply["parameters"]["status"] == "activated"
                assert reply["parameters"]["barrier_id"] == 10
                assert reply["parameters"]["cursor_position"] == {"x": 10.0, "y": 20.0}

                reply = status_conn.receive()
                assert reply["parameters"]["status"] == "deactivated"

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
    def test_subscribe_zones(self, portals, zones):
        with VarlinkConnection() as session_conn:
            instance = session_conn.register()
            session = self.create_session(session_conn)

            with VarlinkConnection() as conn, VarlinkConnection() as zones_conn:
                for c in (conn, zones_conn):
                    c.call(
                        "org.freedesktop.host.portal.Registry.Claim",
                        instance_id=instance,
                    )

                self.start(conn, session)

                zones_conn.send(
                    f"{INPUT_CAPTURE}.SubscribeZones", more=True, session=session
                )

                reply = zones_conn.receive()
                assert "error" not in reply, reply
                assert reply["continues"]

                got = [
                    (z["width"], z["height"], z["x"], z["y"])
                    for z in reply["parameters"]["zones"]
                ]
                assert got == zones
