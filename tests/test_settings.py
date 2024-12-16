# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest


SETTINGS_DATA = {
    "org.freedesktop.appearance": dbus.Dictionary(
        {
            "color-scheme": dbus.UInt32(1),
            "accent-color": dbus.Struct((0.0, 0.1, 0.33), signature="ddd"),
            "contrast": dbus.UInt32(0),
        },
        signature="sv",
    ),
    "org.example.custom": dbus.Dictionary(
        {
            "foo": "bar",
        },
        signature="sv",
    ),
}


@pytest.fixture
def required_templates():
    return {
        "settings": {
            "settings": SETTINGS_DATA,
        },
    }


class TestSettings:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Settings", 2)

    def test_settings_read_all(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")

        value = settings_intf.ReadAll([])
        assert value == SETTINGS_DATA

        value = settings_intf.ReadAll([""])
        assert value == SETTINGS_DATA

        value = settings_intf.ReadAll(["does-not-exist"])
        assert value == {}

        value = settings_intf.ReadAll(["org."])
        assert value == {}

        value = settings_intf.ReadAll(["org.*"])
        assert value == SETTINGS_DATA

        value = settings_intf.ReadAll(
            ["org.freedesktop.appearance", "org.example.custom"]
        )
        assert value == SETTINGS_DATA

        value = settings_intf.ReadAll(["org.freedesktop.appearance"])
        assert len(value) == 1
        assert "org.freedesktop.appearance" in value
        assert (
            value["org.freedesktop.appearance"]
            == SETTINGS_DATA["org.freedesktop.appearance"]
        )

    def test_settings_read(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")

        color_scheme = SETTINGS_DATA["org.freedesktop.appearance"]["color-scheme"]

        value = settings_intf.ReadOne("org.freedesktop.appearance", "color-scheme")
        assert isinstance(value, dbus.UInt32)
        assert value.variant_level == 1
        assert value == color_scheme

        try:
            settings_intf.ReadOne("org.does.not.exist", "color-scheme")
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        try:
            settings_intf.ReadOne("org.freedesktop.appearance", "xcolor-scheme")
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        # deprecated but should still check that it works
        # the crucial detail here is that the variant_level is 2
        value = settings_intf.Read("org.freedesktop.appearance", "color-scheme")
        assert isinstance(value, dbus.UInt32)
        assert value.variant_level == 2
        assert value == color_scheme

    def test_settings_changed(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")
        mock_intf = xdp.get_mock_iface(dbus_con)
        changed_count = 0

        ns = "org.freedesktop.appearance"
        key = "color-scheme"
        current_value = SETTINGS_DATA[ns][key]
        new_value = 2
        assert current_value != new_value

        value = settings_intf.ReadOne(ns, key)
        assert value == current_value

        def cb_settings_changed(changed_ns, changed_key, changed_value):
            nonlocal changed_count
            changed_count += 1
            assert changed_ns == ns
            assert changed_key == key
            assert changed_value == new_value

        settings_intf.connect_to_signal("SettingChanged", cb_settings_changed)
        mock_intf.SetSetting(ns, key, new_value)

        xdp.wait_for(lambda: changed_count == 1)
