# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
import os
from pathlib import Path
from gi.repository import GLib


BACKGROUND_MONITOR_BUS_NAME = "org.freedesktop.background.Monitor"
BACKGROUND_MONITOR_IFACE = "org.freedesktop.background.Monitor"
BACKGROUND_MONITOR_PATH = "/org/freedesktop/background/monitor"
BACKGROUND_STATE = 0


@pytest.fixture
def required_templates():
    return {"background": {}}


@pytest.fixture
def running_flatpak_instance(xdp_app_info_init):
    app_info = xdp_app_info_init
    assert isinstance(app_info, xdp.AppInfoFlatpak)

    instance_dir = (
        Path(os.environ["XDG_RUNTIME_DIR"]) / ".flatpak" / app_info.instance_id
    )
    instance_dir.mkdir(mode=0o700, parents=True)

    pid = os.getpid()
    (instance_dir / "pid").write_text(f"{pid}\n")
    (instance_dir / "bwrapinfo.json").write_text(f'{{"child-pid": {pid}}}\n')
    (instance_dir / "info").write_text(
        f"""
[Application]
name={app_info.app_id}
runtime=org.freedesktop.Platform/x86_64/23.08

[Instance]
instance-id={app_info.instance_id}
"""
    )

    return app_info


@pytest.fixture
def portals_with_running_flatpak(running_flatpak_instance, portals):
    return None


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

    @pytest.mark.parametrize("xdp_app_info", (xdp.AppInfoFlatpak(),))
    @pytest.mark.parametrize(
        "template_params",
        (
            {
                "background": {
                    "app-states": {
                        "org.example.Test": BACKGROUND_STATE,
                    }
                }
            },
        ),
    )
    def test_monitor_lists_background_apps_on_startup(
        self, portals_with_running_flatpak, dbus_con, xdp_app_info
    ):
        obj = dbus_con.get_object(BACKGROUND_MONITOR_BUS_NAME, BACKGROUND_MONITOR_PATH)
        properties_intf = dbus.Interface(obj, "org.freedesktop.DBus.Properties")

        for _ in range(20):
            background_apps = properties_intf.Get(
                BACKGROUND_MONITOR_IFACE, "BackgroundApps"
            )
            if any(app["app_id"] == xdp_app_info.app_id for app in background_apps):
                break

            xdp.wait(100)

        background_apps = properties_intf.Get(
            BACKGROUND_MONITOR_IFACE, "BackgroundApps"
        )
        assert any(app["app_id"] == xdp_app_info.app_id for app in background_apps)

    def test_request_background(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
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

    def test_autostart_desktopfile(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
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

        # Unsupported on snap and linyaps
        if isinstance(xdp_app_info, (xdp.AppInfoSnap, xdp.AppInfoLinyaps)):
            assert response.response == 2
            assert not response.results["autostart"]
            return

        assert response.response == 0
        assert response.results["background"]

        gapp_info = xdp_app_info.gapp_info()
        autostart_name = gapp_info.get_name() if gapp_info else app_id

        assert response.results["autostart"]
        keyfile = self.get_autostart_keyfile(app_id)
        assert keyfile.get_string("Desktop Entry", "Type") == "Application"
        assert keyfile.get_string("Desktop Entry", "Name") == autostart_name
        assert keyfile.get_string("Desktop Entry", "X-XDP-Autostart") == app_id
        assert keyfile.get_boolean("Desktop Entry", "DBusActivatable")

        exec = keyfile.get_string("Desktop Entry", "Exec")
        if isinstance(xdp_app_info, xdp.AppInfoFlatpak):
            assert exec == f"flatpak run --command=/bin/true {app_id} test"
        else:
            assert exec == "/bin/true test"

    def test_autostart_disable(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
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

        # Unsupported on snap and linyaps
        if isinstance(xdp_app_info, (xdp.AppInfoSnap, xdp.AppInfoLinyaps)):
            assert response.response == 2
            assert not response.results["autostart"]
            return

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

    def test_long_reason(self, portals, dbus_con):
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
