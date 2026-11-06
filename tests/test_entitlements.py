# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest
import dbus


def entitlements_good():
    yield (0, [])
    yield (0, ["org.foo.bar"])
    yield (1, ["org.freedesktop.portal.Print"])
    yield (1, ["org.foo.bar", "org.freedesktop.portal.Print"])


def entitlements_bad():
    yield (1, [])
    yield (1, ["org.foo.bar"])


@pytest.fixture
def xdp_app_info(entitlements) -> xdp.AppInfo:
    (version, grants) = entitlements
    return xdp.AppInfoFlatpak(entitlements=grants, entitlements_version=version)


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
