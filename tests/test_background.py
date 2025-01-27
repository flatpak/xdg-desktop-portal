# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest
import os
from pathlib import Path
from gi.repository import GLib


@pytest.fixture
def required_templates():
    return {"background": {}}


class TestBackground:
    def get_autostart_path(self, app_id):
        return Path(os.environ["XDG_CONFIG_HOME"]) / "autostart" / f"{app_id}.desktop"

    def get_autostart_keyfile(self, app_id):
        keyfile = GLib.KeyFile.new()

        desktop_file_path = self.get_autostart_path(app_id)
        with open(str(desktop_file_path.absolute())) as desktop_file:
            desktop_file_contents = desktop_file.read()

            assert keyfile.load_from_data(
                desktop_file_contents,
                len(desktop_file_contents),
                GLib.KeyFileFlags.NONE,
            )

        return keyfile

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Background", 2)

    def test_request_background(self, portals, dbus_con, app_id):
        background_intf = xdp.get_portal_iface(dbus_con, "Background")
        desktop_file = self.get_autostart_path(app_id)

        reason = "Testing portals"

        request = xdp.Request(dbus_con, background_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "RequestBackground",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["background"]
        assert not response.results["autostart"]

        assert not desktop_file.exists()

    def test_autostart_desktopfile(self, portals, dbus_con, app_id):
        background_intf = xdp.get_portal_iface(dbus_con, "Background")

        reason = "Testing portals"
        autostart = True
        commandline = ["/bin/true", "test"]
        dbus_activatable = True

        request = xdp.Request(dbus_con, background_intf)
        options = {
            "reason": reason,
            "autostart": autostart,
            "commandline": commandline,
            "dbus-activatable": dbus_activatable,
        }
        response = request.call(
            "RequestBackground",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["background"]
        assert response.results["autostart"]

        keyfile = self.get_autostart_keyfile(app_id)
        assert keyfile.get_string("Desktop Entry", "Type") == "Application"
        assert keyfile.get_string("Desktop Entry", "Name") == app_id
        assert keyfile.get_string("Desktop Entry", "X-XDP-Autostart") == app_id
        assert keyfile.get_string("Desktop Entry", "Exec") == "/bin/true test"
        assert keyfile.get_boolean("Desktop Entry", "DBusActivatable")

    def test_autostart_disable(self, portals, dbus_con, app_id):
        background_intf = xdp.get_portal_iface(dbus_con, "Background")
        desktop_file = self.get_autostart_path(app_id)

        reason = "Testing portals"
        autostart = True

        request = xdp.Request(dbus_con, background_intf)
        options = {
            "reason": reason,
            "autostart": autostart,
        }
        response = request.call(
            "RequestBackground",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["background"]
        assert response.results["autostart"]

        assert desktop_file.exists()

        request = xdp.Request(dbus_con, background_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "RequestBackground",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["background"]
        assert not response.results["autostart"]

        assert not desktop_file.exists()

    def test_long_reason(self, portals, dbus_con, app_id):
        background_intf = xdp.get_portal_iface(dbus_con, "Background")

        reason = (
            "012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
            + "012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
            + "012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
        )
        autostart = True
        commandline = ["/bin/true", "test"]
        dbus_activatable = True

        request = xdp.Request(dbus_con, background_intf)
        options = {
            "reason": reason,
            "autostart": autostart,
            "commandline": commandline,
            "dbus-activatable": dbus_activatable,
        }
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "RequestBackground",
                parent_window="",
                options=options,
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )
