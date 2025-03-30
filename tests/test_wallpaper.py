# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest
import os
import tempfile
from pathlib import Path


@pytest.fixture
def required_templates():
    return {"wallpaper": {}}


class TestWallpaper:
    def set_permission(self, dbus_con, app_id, permission):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetPermission(
            "wallpaper",
            True,
            "wallpaper",
            app_id,
            [permission],
        )

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Wallpaper", 1)

    def test_wallpaper_uri(self, portals, dbus_con, app_id):
        wallpaper_intf = xdp.get_portal_iface(dbus_con, "Wallpaper")
        mock_intf = xdp.get_mock_iface(dbus_con)

        uri = "file:///test"
        show_preview = True
        set_on = "both"

        request = xdp.Request(dbus_con, wallpaper_intf)
        options = {
            "show-preview": show_preview,
            "set-on": set_on,
        }
        response = request.call(
            "SetWallpaperURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SetWallpaperURI")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == uri
        assert args[4]["show-preview"] == show_preview
        assert args[4]["set-on"] == set_on

    def test_wallpaper_file(self, portals, dbus_con, app_id):
        wallpaper_intf = xdp.get_portal_iface(dbus_con, "Wallpaper")
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd, _ = tempfile.mkstemp(prefix="wallpaper_mock", dir=Path.home())
        os.write(fd, b"wallpaper_mock_file")

        show_preview = True

        request = xdp.Request(dbus_con, wallpaper_intf)
        options = {
            "show-preview": show_preview,
        }
        response = request.call(
            "SetWallpaperFile",
            parent_window="",
            fd=fd,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SetWallpaperURI")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[4]["show-preview"] == show_preview

        path = args[3]
        assert path.startswith("file:///")

        with open(path[7:]) as file:
            wallpaper_file_contents = file.read()
            assert wallpaper_file_contents == "wallpaper_mock_file"

    @pytest.mark.parametrize("template_params", ({"wallpaper": {"response": 1}},))
    def test_wallpaper_cancel(self, portals, dbus_con, app_id):
        wallpaper_intf = xdp.get_portal_iface(dbus_con, "Wallpaper")

        uri = "file:///test"
        show_preview = True
        set_on = "both"

        request = xdp.Request(dbus_con, wallpaper_intf)
        options = {
            "show-preview": show_preview,
            "set-on": set_on,
        }
        response = request.call(
            "SetWallpaperURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        assert response
        assert response.response == 1

    def test_wallpaper_permission(self, portals, dbus_con, app_id):
        wallpaper_intf = xdp.get_portal_iface(dbus_con, "Wallpaper")
        mock_intf = xdp.get_mock_iface(dbus_con)

        self.set_permission(dbus_con, app_id, "no")

        uri = "file:///test"
        show_preview = True
        set_on = "both"

        request = xdp.Request(dbus_con, wallpaper_intf)
        options = {
            "show-preview": show_preview,
            "set-on": set_on,
        }
        response = request.call(
            "SetWallpaperURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        assert response
        assert response.response == 2

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SetWallpaperURI")
        assert len(method_calls) == 0
