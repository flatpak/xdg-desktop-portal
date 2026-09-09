# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import dbus
import pytest

import tests.xdp_utils as xdp
from tests.test_varlink import VarlinkConnection

SETTINGS_DATA_TEST1 = {
    "org.freedesktop.appearance": dbus.Dictionary(
        {
            "color-scheme": dbus.UInt32(1),
            "accent-color": dbus.Struct((0.0, 0.1, 0.33), signature="ddd"),
        },
        signature="sv",
    ),
}

SETTINGS_DATA_TEST2 = {
    "org.freedesktop.appearance": dbus.Dictionary(
        {
            "color-scheme": dbus.UInt32(2),
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

SETTINGS_DATA_BAD = {
    "org.freedesktop.appearance": dbus.Dictionary(
        {
            "color-scheme": dbus.UInt32(99),
            "accent-color": dbus.Struct((11.11, 22.22, 33.33), signature="ddd"),
        },
        signature="sv",
    ),
    "org.example.custom": dbus.Dictionary(
        {
            "foo": "baz",
        },
        signature="sv",
    ),
    "org.example.custom.bad": dbus.Dictionary(
        {
            "bad": "bad",
        },
        signature="sv",
    ),
}

# This is the expected data, merged SETTINGS_DATA_TEST1 and SETTINGS_DATA_TEST2
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
        "settings:org.freedesktop.impl.portal.Test1": {
            "settings": SETTINGS_DATA_TEST1,
        },
        "settings:org.freedesktop.impl.portal.Test2": {
            "settings": SETTINGS_DATA_TEST2,
        },
        "settings:org.freedesktop.impl.portal.TestBad": {
            "settings": SETTINGS_DATA_BAD,
        },
    }


PORTAL_CONFIG_FILES = {
    "test1.portal": b"""
[portal]
DBusName=org.freedesktop.impl.portal.Test1
Interfaces=org.freedesktop.impl.portal.Settings;
""",
    "test2.portal": b"""
[portal]
DBusName=org.freedesktop.impl.portal.Test2
Interfaces=org.freedesktop.impl.portal.Settings;
""",
    "test_bad.portal": b"""
[portal]
DBusName=org.freedesktop.impl.portal.TestBad
Interfaces=org.freedesktop.impl.portal.Settings;
""",
    "test_noimpl.portal": b"""
[portal]
DBusName=org.freedesktop.impl.portal.TestBad
Interfaces=org.freedesktop.impl.portal.NonExistant;
""",
}


def portal_config_good():
    # test1 merged with test2 should result in the correct output
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test1;test2;
"""
    yield files

    # a portal without the settings impl does not affect the result
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test1;test_noimpl;test2;
"""
    yield files

    # the default should be ignored when the interface is configured
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test_bad;
org.freedesktop.impl.portal.Settings=test1;test2
"""
    yield files

    # use * which should expand to test1;test2;test_noimpl
    files = PORTAL_CONFIG_FILES.copy()
    del files["test_bad.portal"]
    files["test-portals.conf"] = b"""
[preferred]
default=test_noimpl;
org.freedesktop.impl.portal.Settings=*;
"""
    yield files


def portal_config_bad():
    # test1 alone should result in bad output
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test1;
"""
    yield files

    # test2 merged with test1 is the wrong order
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test2;test1;
"""
    yield files

    # test_noimpl does not affect anything
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test_noimpl;test2;test1;
"""
    yield files

    # default should get ignored, test2 alone should result in bad output
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test1;test2
org.freedesktop.impl.portal.Settings=test2;test_noimpl
"""
    yield files

    # test_bad anywhere in the active config should result in bad output
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test1;test2
org.freedesktop.impl.portal.Settings=test_bad;test1;test2
"""
    yield files

    # use * which expands to test1;test2;test_bad;test_no_impl
    # contains test_bad which should result in bad output
    files = PORTAL_CONFIG_FILES.copy()
    files["test-portals.conf"] = b"""
[preferred]
default=test_noimpl;
org.freedesktop.impl.portal.Settings=*;
"""
    yield files


def portal_config_twice():
    # check that test1 gets picked up only once
    files = PORTAL_CONFIG_FILES.copy()
    del files["test_bad.portal"]
    files["test-portals.conf"] = b"""
[preferred]
default=test_noimpl;
org.freedesktop.impl.portal.Settings=test1;*;
"""
    yield files


@pytest.fixture
def xdg_desktop_portal_dir_default_files():
    return next(portal_config_good())


class TestSettings:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Settings", 2)

    @pytest.mark.parametrize(
        "xdg_desktop_portal_dir_default_files",
        portal_config_good(),
    )
    def test_read_all(self, portals, dbus_con):
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

    @pytest.mark.parametrize(
        "xdg_desktop_portal_dir_default_files",
        portal_config_bad(),
    )
    def test_read_all_bad_config(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")

        value = settings_intf.ReadAll([])
        assert value != SETTINGS_DATA

    @pytest.mark.parametrize(
        "xdg_desktop_portal_dir_default_files",
        portal_config_twice(),
    )
    def test_config_twice(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")
        mock_intf = xdp.get_mock_iface(dbus_con, "org.freedesktop.impl.portal.Test1")

        value = settings_intf.ReadAll([])
        assert value == SETTINGS_DATA

        # The config is `test1;*`, make sure we only get a single call to Test1
        method_calls = mock_intf.GetMethodCalls("ReadAll")
        assert len(method_calls) == 1

    def test_read(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")

        color_scheme = SETTINGS_DATA["org.freedesktop.appearance"]["color-scheme"]

        value = settings_intf.ReadOne("org.freedesktop.appearance", "color-scheme")
        assert isinstance(value, dbus.UInt32)
        assert value.variant_level == 1
        assert value == color_scheme

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            settings_intf.ReadOne("org.does.not.exist", "color-scheme")
        assert excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            settings_intf.ReadOne("org.freedesktop.appearance", "xcolor-scheme")
        assert excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        # deprecated but should still check that it works
        # the crucial detail here is that the variant_level is 2
        value = settings_intf.Read("org.freedesktop.appearance", "color-scheme")
        assert isinstance(value, dbus.UInt32)
        assert value.variant_level == 2
        assert value == color_scheme

    def test_changed(self, portals, dbus_con):
        settings_intf = xdp.get_portal_iface(dbus_con, "Settings")
        mock_intf = xdp.get_mock_iface(dbus_con, "org.freedesktop.impl.portal.Test1")
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


SETTINGS = "org.freedesktop.portal.Settings"

# SETTINGS_DATA as the varlink interface reports it
VARLINK_SETTINGS_DATA = {
    "org.freedesktop.appearance": {
        "color-scheme": {"v": 1},
        "accent-color": {"v": [0.0, 0.1, 0.33]},
        "contrast": {"v": 0},
    },
    "org.example.custom": {
        "foo": {"v": "bar"},
    },
}


def read_all(conn, namespaces):
    reply = conn.call(f"{SETTINGS}.ReadAll", namespaces=namespaces)
    assert "error" not in reply, reply
    return {ns["namespace"]: ns["values"] for ns in reply["parameters"]["values"]}


class TestVarlinkSettings:
    @pytest.fixture
    def xdp_app_info(self) -> xdp.AppInfo:
        return xdp.AppInfoHost()

    def test_read_all(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            assert read_all(conn, []) == VARLINK_SETTINGS_DATA
            assert read_all(conn, [""]) == VARLINK_SETTINGS_DATA
            assert read_all(conn, ["org.*"]) == VARLINK_SETTINGS_DATA
            assert read_all(conn, ["does-not-exist"]) == {}
            assert read_all(conn, ["org.freedesktop.appearance"]) == {
                "org.freedesktop.appearance": VARLINK_SETTINGS_DATA[
                    "org.freedesktop.appearance"
                ]
            }

    def test_read(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply = conn.call(
                f"{SETTINGS}.Read",
                namespace="org.freedesktop.appearance",
                key="color-scheme",
            )
            assert reply["parameters"]["value"] == {"v": 1}

            reply = conn.call(
                f"{SETTINGS}.Read",
                namespace="org.freedesktop.appearance",
                key="xcolor-scheme",
            )
            assert reply["error"] == f"{SETTINGS}.KeyNotFound"

    def test_read_before_registering(self, portals):
        with VarlinkConnection() as conn:
            reply = conn.call(f"{SETTINGS}.ReadAll", namespaces=[])

        assert reply["error"] == f"{SETTINGS}.NotRegistered"

    def test_subscribe_without_more(self, portals):
        with VarlinkConnection() as conn:
            conn.register()
            reply = conn.call(f"{SETTINGS}.SubscribeSettingChanged", namespaces=[])

        assert reply["error"] == f"{SETTINGS}.ExpectedMore"

    def test_subscribe(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con, "org.freedesktop.impl.portal.Test1")
        ns = "org.freedesktop.appearance"

        with VarlinkConnection() as conn:
            conn.register()
            conn.send(
                f"{SETTINGS}.SubscribeSettingChanged", more=True, namespaces=[ns]
            )

            # The current value of every setting in the namespace comes first
            enumerated = {}
            for _ in range(len(VARLINK_SETTINGS_DATA[ns])):
                reply = conn.receive()
                assert reply["continues"]
                assert reply["parameters"]["namespace"] == ns
                enumerated[reply["parameters"]["key"]] = reply["parameters"]["value"]

            assert enumerated == VARLINK_SETTINGS_DATA[ns]

            mock_intf.SetSetting(ns, "color-scheme", 2)

            reply = conn.receive()
            assert reply["continues"]
            assert reply["parameters"] == {
                "namespace": ns,
                "key": "color-scheme",
                "value": {"v": 2},
            }

    def test_subscribe_filters_by_namespace(self, portals, dbus_con):
        test1 = xdp.get_mock_iface(dbus_con, "org.freedesktop.impl.portal.Test1")
        test2 = xdp.get_mock_iface(dbus_con, "org.freedesktop.impl.portal.Test2")

        with VarlinkConnection() as conn:
            conn.register()
            conn.send(
                f"{SETTINGS}.SubscribeSettingChanged",
                more=True,
                namespaces=["org.example.*"],
            )

            reply = conn.receive()
            assert reply["parameters"]["namespace"] == "org.example.custom"

            test1.SetSetting("org.freedesktop.appearance", "color-scheme", 2)
            test2.SetSetting("org.example.custom", "foo", "baz")

            reply = conn.receive()
            assert reply["parameters"] == {
                "namespace": "org.example.custom",
                "key": "foo",
                "value": {"v": "baz"},
            }
