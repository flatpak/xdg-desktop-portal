# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest
import dbus
import os
from pathlib import Path


SVG_IMAGE_DATA = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" height="16px" width="16px"/>
"""

DESKTOP_FILE = b"""[Desktop Entry]
Version=1.0
Name=Dynamic Launcher Example
Exec=true %u
Type=Application
"""


@pytest.fixture
def required_templates():
    return {"dynamiclauncher": {}}


class TestDynamicLauncher:
    def test_version(self, portals, dbus_con):
        """tests the version of the interface"""

        xdp.check_version(dbus_con, "DynamicLauncher", 1)

    def test_basic(self, portals, dbus_con, app_id):
        """test that the backend receives the expected data"""

        dynlauncher_intf = xdp.get_portal_iface(dbus_con, "DynamicLauncher")
        mock_intf = xdp.get_mock_iface(dbus_con)

        app_name = "App Name"
        bytes = SVG_IMAGE_DATA.encode("utf-8")

        request = xdp.Request(dbus_con, dynlauncher_intf)
        options = {
            "modal": False,
        }
        response = request.call(
            "PrepareInstall",
            parent_window="",
            name=app_name,
            icon_v=dbus.Struct(
                ("bytes", dbus.ByteArray(bytes, variant_level=1)),
                signature="sv",
                variant_level=1,
            ),
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["name"] == app_name
        token = response.results["token"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PrepareInstall")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3] == app_name  # name
        # args[4] == icon
        assert not args[5]["modal"]

        desktop_file_name = app_id + ".ExampleApp.desktop"
        dynlauncher_intf.Install(
            token,
            desktop_file_name,
            DESKTOP_FILE,
            {},
        )

        file = Path(os.environ["XDG_DATA_HOME"]) / "applications" / desktop_file_name
        assert file.exists()
