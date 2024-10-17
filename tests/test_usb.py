# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests import Session
from gi.repository import GLib

import pytest
import os
import gi

gi.require_version("UMockdev", "1.0")
from gi.repository import UMockdev  # noqa E402


@pytest.fixture
def portal_name():
    return "Usb"


@pytest.fixture
def umockdev():
    return UMockdev.Testbed.new()


class TestUsb:
    _num_devices = 0

    def generate_device(
        self, testbed, vendor, vendor_name, product, product_name, serial
    ):
        n = self._num_devices
        self._num_devices += 1

        testbed.add_from_string(f"""P: /devices/usb{n}
N: bus/usb/001/{n:03d}
E: BUSNUM=001
E: DEVNUM={n:03d}
E: DEVNAME=/dev/bus/usb/001/{n:03d}
E: DEVTYPE=usb_device
E: DRIVER=usb
E: ID_BUS=usb
E: ID_MODEL={product_name}
E: ID_MODEL_ID={product}
E: ID_REVISION=0002
E: ID_SERIAL={vendor_name}_{product_name}_{serial}
E: ID_SERIAL_SHORT={serial}
E: ID_VENDOR={vendor_name}
E: ID_VENDOR_ID={vendor}
E: SUBSYSTEM=usb
A: idProduct={product}
A: idVendor={vendor}
""")

        return f"/sys/devices/usb{n}"

    def test_version(self, portal_mock):
        portal_mock.check_version(1)

    def test_create_close_session(self, portal_mock, app_id):
        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        session.close()

    def test_empty_initial_devices(self, portal_mock, app_id):
        device_events_signal_received = False

        usb_intf = portal_mock.get_dbus_interface()

        Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        def cb_device_events(session_handle, events):
            nonlocal device_events_signal_received
            device_events_signal_received = True

        usb_intf.connect_to_signal("DeviceEvents", cb_device_events)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        assert not device_events_signal_received

    @pytest.mark.parametrize("usb_queries", ["vnd:04a9", None])
    def test_initial_devices(self, portal_mock, app_id, usb_queries):
        device_events_signal_received = False
        devices_received = 0

        self.generate_device(
            portal_mock.umockdev,
            "04a9",
            "Canon_Inc.",
            "31c0",
            "Canon_Digital_Camera",
            "C767F1C714174C309255F70E4A7B2EE2",
        )

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        def cb_device_events(session_handle, events):
            nonlocal device_events_signal_received
            nonlocal devices_received
            assert session.handle == session_handle

            for action, id, device in events:
                assert action == "add"
                devices_received += 1

            device_events_signal_received = True

        usb_intf.connect_to_signal("DeviceEvents", cb_device_events)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        if usb_queries is None:
            assert not device_events_signal_received
            assert devices_received == 0
        else:
            assert device_events_signal_received
            assert devices_received == 1

    @pytest.mark.parametrize("usb_queries", ["vnd:04a9", None])
    def test_device_add(self, portal_mock, app_id, usb_queries):
        device_events_signal_received = False
        devices_received = 0
        device = None

        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        def cb_device_events(session_handle, events):
            nonlocal device_events_signal_received
            nonlocal devices_received
            nonlocal device
            assert session.handle == session_handle

            for action, _, dev in events:
                assert action == "add"
                device = dev
                devices_received += 1

            device_events_signal_received = True

        usb_intf.connect_to_signal("DeviceEvents", cb_device_events)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        assert not device_events_signal_received

        self.generate_device(
            portal_mock.umockdev,
            "04a9",
            "Canon_Inc.",
            "31c0",
            "Canon_Digital_Camera",
            "C767F1C714174C309255F70E4A7B2EE2",
        )

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        if usb_queries is None:
            assert not device_events_signal_received
            assert devices_received == 0
        else:
            assert device_events_signal_received
            assert devices_received == 1

            assert device
            assert device["readable"]
            assert device["writable"]
            assert device["device-file"] == "/dev/bus/usb/001/000"
            assert device["properties"]["ID_VENDOR_ID"] == "04a9"
            assert device["properties"]["ID_MODEL_ID"] == "31c0"
            assert (
                device["properties"]["ID_SERIAL"]
                == "Canon_Inc._Canon_Digital_Camera_C767F1C714174C309255F70E4A7B2EE2"
            )

    @pytest.mark.parametrize("usb_queries", ["vnd:04a9", None])
    def test_device_remove(self, portal_mock, app_id, usb_queries):
        device_events_signal_count = 0
        devices_received = 0
        devices_removed = 0

        dev_path = self.generate_device(
            portal_mock.umockdev,
            "04a9",
            "Canon_Inc.",
            "31c0",
            "Canon_Digital_Camera",
            "C767F1C714174C309255F70E4A7B2EE2",
        )

        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        def cb_device_events(session_handle, events):
            nonlocal device_events_signal_count
            nonlocal devices_received
            nonlocal devices_removed

            assert session.handle == session_handle

            for action, id, device in events:
                if action == "add":
                    devices_received += 1
                elif action == "remove":
                    devices_removed += 1
                else:
                    assert False

            device_events_signal_count += 1

        usb_intf.connect_to_signal("DeviceEvents", cb_device_events)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        if usb_queries is None:
            assert device_events_signal_count == 0
            assert devices_received == 0
            assert devices_removed == 0
        else:
            assert device_events_signal_count == 1
            assert devices_received == 1
            assert devices_removed == 0

        portal_mock.umockdev.remove_device(dev_path)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        if usb_queries is None:
            assert device_events_signal_count == 0
            assert devices_received == 0
            assert devices_removed == 0
        else:
            assert device_events_signal_count == 2
            assert devices_received == 1
            assert devices_removed == 1

    @pytest.mark.parametrize("usb_queries", ["vnd:04a9;vnd:04aa"])
    @pytest.mark.parametrize("params", [{"filters": {"vendor": "04a9"}}])
    def test_acquire(self, portal_mock, app_id):
        self.generate_device(
            portal_mock.umockdev,
            "04a9",
            "Canon_Inc.",
            "31c0",
            "Canon_Digital_Camera",
            "C767F1C714174C309255F70E4A7B2EE2",
        )

        self.generate_device(
            portal_mock.umockdev,
            "04aa",
            "Someone Else.",
            "31c0",
            "SomeProduct",
            "00001",
        )

        possible_vendors = ["04a9", "04aa"]

        usb_intf = portal_mock.get_dbus_interface()
        devices = usb_intf.EnumerateDevices({})
        assert len(devices) == 2
        (id1, dev_info1) = devices[0]
        assert id1
        assert dev_info1
        vendor_id = dev_info1["properties"]["ID_VENDOR_ID"]
        assert vendor_id in possible_vendors
        possible_vendors.remove(vendor_id)
        (id2, dev_info2) = devices[1]
        assert id2
        assert dev_info2
        vendor_id = dev_info2["properties"]["ID_VENDOR_ID"]
        assert vendor_id in possible_vendors
        possible_vendors.remove(vendor_id)

        request = portal_mock.create_request()
        response = request.call(
            "AcquireDevices",
            parent_window="",
            devices=[
                (id1, {"writable": True}),
                (id2, {"writable": True}),
            ],
            options={},
        )
        assert response.response == 0

        (results, finished) = usb_intf.FinishAcquireDevices(request.handle, {})
        assert finished
        assert len(results) == 1
        (res_id, device) = results[0]
        assert res_id == id1 or res_id == id2
        assert device["success"]
        fd = device["fd"].take()
        assert fd > 0
        with os.fdopen(fd, "r") as f:
            assert f
        assert "error" not in device

        usb_intf.ReleaseDevices([res_id], {})

    @pytest.mark.parametrize("usb_queries", ["vnd:0001"])
    @pytest.mark.parametrize(
        "expected,params",
        [
            (1, {"filters": {"model": "0000"}}),
            (1, {"filters": {"model": "0001"}}),
            (0, {"filters": {"model": "0002"}}),
            (2, {"filters": {"vendor": "0001"}}),
            (0, {"filters": {"vendor": "0002"}}),
            (1, {"filters": {"vendor": "0001", "model": "0000"}}),
            (0, {"filters": {"vendor": "0002", "model": "0000"}}),
        ],
    )
    def test_queries(self, expected, portal_mock, app_id, usb_queries):
        for i in range(2):
            self.generate_device(
                portal_mock.umockdev,
                "0001",
                "example_org",
                f"000{i}",
                f"model{i}",
                "0001",
            )

        usb_intf = portal_mock.get_dbus_interface()
        devices = usb_intf.EnumerateDevices({})
        assert len(devices) == 2
        acquire_devices = [(id, {"writable": True}) for (id, _) in devices]

        request = portal_mock.create_request()
        response = request.call(
            "AcquireDevices",
            parent_window="",
            devices=acquire_devices,
            options={},
        )
        assert response.response == 0

        (results, finished) = usb_intf.FinishAcquireDevices(request.handle, {})
        assert finished
        assert len(results) == expected
