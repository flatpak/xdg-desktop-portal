# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
from typing import Any


SCREENSHOT_DATA = dbus.Dictionary(
    {
        "uri": "file:///screenshot.png",
        "color": (0.0, 1.0, 0.331),
    },
    signature="sv",
)


@pytest.fixture
def required_templates():
    return {
        "access": {},
        "screenshot": {
            "results": SCREENSHOT_DATA,
        },
    }


class TestScreenshot:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Screenshot", 2)

    @pytest.mark.parametrize("modal", [True, False])
    @pytest.mark.parametrize("interactive", [True, False])
    def test_screenshot_basic(
        self, portals, dbus_con, xdp_app_info, modal, interactive
    ):
        app_id = xdp_app_info.app_id
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, screenshot_intf)
        options = {
            "modal": modal,
            "interactive": interactive,
        }
        response = request.call(
            "Screenshot",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uri"] == SCREENSHOT_DATA["uri"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Screenshot")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3]["modal"] == modal
        assert args[3]["interactive"] == interactive

        # check that args were forwarded to access portal correctly
        if not interactive:
            method_calls = mock_intf.GetMethodCalls("AccessDialog")
            assert len(method_calls) > 0
            _, args = method_calls[-1]
            assert args[1] == app_id
            assert args[2] == ""  # parent window
            assert args[6]["modal"] == modal

    @pytest.mark.parametrize(
        "template_params", ({"screenshot": {"expect-close": True}},)
    )
    def test_screenshot_close(self, portals, dbus_con):
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")

        request = xdp.Request(dbus_con, screenshot_intf)
        request.schedule_close(1000)
        options = {
            "interactive": True,
        }
        request.call(
            "Screenshot",
            parent_window="",
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

    @pytest.mark.parametrize("template_params", ({"screenshot": {"response": 1}},))
    def test_screenshot_cancel(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        modal = True
        interactive = True

        request = xdp.Request(dbus_con, screenshot_intf)
        options = {
            "modal": modal,
            "interactive": interactive,
        }
        response = request.call(
            "Screenshot",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Screenshot")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3]["modal"] == modal
        assert args[3]["interactive"] == interactive

    def test_pick_color_basic(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, screenshot_intf)
        options: Any = {}
        response = request.call(
            "PickColor",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["color"] == SCREENSHOT_DATA["color"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PickColor")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window

    @pytest.mark.parametrize(
        "template_params", ({"screenshot": {"expect-close": True}},)
    )
    def test_pick_color_close(self, portals, dbus_con):
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")

        request = xdp.Request(dbus_con, screenshot_intf)
        request.schedule_close(1000)
        options: Any = {}
        request.call(
            "PickColor",
            parent_window="",
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

    @pytest.mark.parametrize("template_params", ({"screenshot": {"response": 1}},))
    def test_pick_color_cancel(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, screenshot_intf)
        options: Any = {}
        response = request.call(
            "PickColor",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PickColor")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
