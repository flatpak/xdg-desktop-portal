# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest
import dbus


@pytest.fixture
def required_templates():
    return {
        "parental_controls": {
            "results": {"low": dbus.Int32(13), "high": dbus.Int32(16)},
        },
    }


class TestParentalControls:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "ParentalControls", 1)

    def test_basic1(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        pc_intf = xdp.get_portal_iface(dbus_con, "ParentalControls")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, pc_intf)
        response = request.call(
            "QueryAgeBracket",
            window="",
            gates=[13, 16, 18],
            options={},
        )

        assert response
        assert response.response == 0
        assert int(response.results["low"]) == 13
        assert int(response.results["high"]) == 16


    @pytest.mark.parametrize("template_params", ({"parental_controls": {"expect-close": True}},))
    def test_close(self, portals, dbus_con):
        pc_intf = xdp.get_portal_iface(dbus_con, "ParentalControls")

        request = xdp.Request(dbus_con, pc_intf)
        request.schedule_close(1000)
        request.call(
            "QueryAgeBracket",
            window="",
            gates=[13, 21],
            options={},
        )

        # Only true if the impl.Request was closed too
        assert request.closed

