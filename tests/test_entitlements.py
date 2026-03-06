# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest
import dbus
from enum import Flag


class StrictEntitlements(Flag):
    ENABLED = True
    DISABLED = False


def entitlements_good():
    yield (StrictEntitlements.DISABLED, [])
    yield (StrictEntitlements.DISABLED, ["org.foo.bar"])
    yield (StrictEntitlements.ENABLED, ["org.freedesktop.portal.Print"])
    yield (StrictEntitlements.ENABLED, ["org.foo.bar", "org.freedesktop.portal.Print"])


def entitlements_bad():
    yield (StrictEntitlements.ENABLED, [])
    yield (StrictEntitlements.ENABLED, ["org.foo.bar"])


@pytest.fixture
def xdp_app_info(entitlements) -> xdp.AppInfo:
    (strict, entitlements) = entitlements
    return xdp.AppInfoFlatpak(entitlements=entitlements, strict_entitlements=strict)


@pytest.fixture
def required_templates():
    return {
        "print": {},
        "lockdown": {},
    }


class TestEntitlements:
    @pytest.mark.parametrize("entitlements", entitlements_good())
    def test_entitlements_good(self, portals, dbus_con, xdp_app_info):
        print_intf = xdp.get_portal_iface(dbus_con, "Print")

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "PreparePrint",
            parent_window="",
            title="Foo",
            settings={},
            page_setup={},
            options={},
        )

        assert response
        assert response.response == 0

    @pytest.mark.parametrize("entitlements", entitlements_bad())
    def test_entitlements_bad(self, portals, dbus_con, xdp_app_info):
        print_intf = xdp.get_portal_iface(dbus_con, "Print")

        request = xdp.Request(dbus_con, print_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "PreparePrint",
                parent_window="",
                title="Foo",
                settings={},
                page_setup={},
                options={},
            )
        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )
