# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
from enum import Flag
from typing import Any
import os
from pathlib import Path


SCREENSHOT_DATA = dbus.Dictionary(
    {
        "uri": "file:///screenshot.png",
        "color": (0.0, 1.0, 0.331),
    },
    signature="sv",
)


class ScreenshotTarget(Flag):
    SCREEN = 1
    WINDOW = 2
    AREA = 4
    ACTIVE_WINDOW = 8


SCREENSHOT_TARGETS_GOOD = tuple(target.value for target in ScreenshotTarget)
SCREENSHOT_TARGETS_BAD = (
    0,
    (ScreenshotTarget.SCREEN | ScreenshotTarget.WINDOW).value,
    16,
)
SCREENSHOT_TARGETS_ALL = (
    ScreenshotTarget.SCREEN
    | ScreenshotTarget.WINDOW
    | ScreenshotTarget.AREA
    | ScreenshotTarget.ACTIVE_WINDOW
).value


@pytest.fixture
def required_templates():
    image = Path(os.environ["XDG_DATA_HOME"]) / "screenshot-image.png"
    image.write_text("image contents")
    SCREENSHOT_DATA["uri"] = f"file://{image.absolute().as_posix()}"

    return {
        "access": {},
        "screenshot": {
            "results": SCREENSHOT_DATA,
        },
    }


class TestScreenshot:
    def set_permission(self, dbus_con, appid, permission):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetPermission(
            "screenshot",
            True,
            "screenshot",
            appid,
            [permission],
        )

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Screenshot", 3)

    def test_available_targets(self, portals, dbus_con):
        properties_intf = dbus.Interface(
            xdp.get_xdp_dbus_object(dbus_con), "org.freedesktop.DBus.Properties"
        )
        available_targets = properties_intf.Get(
            "org.freedesktop.portal.Screenshot",
            "AvailableTargets",
        )
        assert int(available_targets) == SCREENSHOT_TARGETS_ALL

    @pytest.mark.parametrize("target", SCREENSHOT_TARGETS_GOOD)
    def test_screenshot_target(
        self, xdg_document_portal, portals, dbus_con, xdp_app_info, target
    ):
        app_id = xdp_app_info.app_id
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, screenshot_intf)
        response = request.call(
            "Screenshot",
            parent_window="",
            options={
                "interactive": True,
                "target": dbus.UInt32(target),
            },
        )

        assert response
        assert response.response == 0

        method_calls = mock_intf.GetMethodCalls("Screenshot")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""
        assert args[3]["target"] == target

    @pytest.mark.parametrize("target", SCREENSHOT_TARGETS_BAD)
    def test_screenshot_invalid_target(self, portals, dbus_con, target):
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")

        request = xdp.Request(dbus_con, screenshot_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "Screenshot",
                parent_window="",
                options={
                    "interactive": True,
                    "target": dbus.UInt32(target),
                },
            )

        e = excinfo.value
        assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        assert "Invalid screenshot target" in e.get_dbus_message()

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "screenshot": {
                    "available-targets": ScreenshotTarget.SCREEN.value,
                    "results": SCREENSHOT_DATA,
                },
            },
        ),
    )
    def test_screenshot_unavailable_target(self, portals, dbus_con):
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")

        request = xdp.Request(dbus_con, screenshot_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "Screenshot",
                parent_window="",
                options={
                    "interactive": True,
                    "target": dbus.UInt32(ScreenshotTarget.WINDOW.value),
                },
            )

        e = excinfo.value
        assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        assert "Unavailable screenshot target" in e.get_dbus_message()

    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "screenshot": {
                    "version": 2,
                    "results": SCREENSHOT_DATA,
                },
            },
        ),
    )
    def test_screenshot_options_forwarded_to_v2_backend(
        self, xdg_document_portal, portals, dbus_con
    ):
        xdp.check_version(dbus_con, "Screenshot", 2)

        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, screenshot_intf)
        response = request.call(
            "Screenshot",
            parent_window="",
            options={
                "modal": False,
                "interactive": True,
            },
        )

        assert response
        assert response.response == 0

        method_calls = mock_intf.GetMethodCalls("Screenshot")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert not args[3]["modal"]
        assert args[3]["interactive"]
        assert "target" not in args[3]

    @pytest.mark.parametrize("modal", [True, False])
    @pytest.mark.parametrize("interactive", [True, False])
    def test_screenshot_basic(
        self, xdg_document_portal, portals, dbus_con, xdp_app_info, modal, interactive
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

        assert xdp.uri_same_file(SCREENSHOT_DATA["uri"], response.results["uri"])

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

    @pytest.mark.parametrize("permission", ["", "yes", "no", "ask"])
    def test_screenshot_permissions(
        self, xdg_document_portal, portals, dbus_con, xdp_app_info, permission
    ):
        app_id = xdp_app_info.app_id
        screenshot_intf = xdp.get_portal_iface(dbus_con, "Screenshot")
        mock_intf = xdp.get_mock_iface(dbus_con)

        self.set_permission(dbus_con, app_id, permission)

        request = xdp.Request(dbus_con, screenshot_intf)
        options = {
            "modal": True,
            "interactive": False,
            "handle_token": request.handle_token,
        }
        response = request.call(
            "Screenshot",
            parent_window="",
            options=options,
        )

        assert response
        if permission == "no":
            assert response.response != 0
            return

        assert response.response == 0
        method_calls = mock_intf.GetMethodCalls("AccessDialog")
        if permission == "yes":
            assert len(method_calls) == 0
        else:
            assert len(method_calls) > 0

        request = xdp.Request(dbus_con, screenshot_intf)
        options["handle_token"] = request.handle_token
        response = request.call(
            "Screenshot",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        method_calls_2 = mock_intf.GetMethodCalls("AccessDialog")
        if permission == "ask":
            assert len(method_calls_2) > len(method_calls)
        else:
            assert len(method_calls_2) == len(method_calls)

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
