# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

import os
from pathlib import Path

import dbus
import pytest

import tests.xdp_utils as xdp

CORRECT_APP_ID_DESKTOP = b"""
[Desktop Entry]
Version=1.0
Name=CorrectAppId
Exec=true %u
Type=Application
"""


def create_profile(profiles: Path, name: str, app_ids: list[str]) -> Path:
    profile = profiles / name
    applications = profile / "share" / "applications"
    applications.mkdir(parents=True)
    for app_id in app_ids:
        (applications / f"{app_id}.desktop").write_bytes(CORRECT_APP_ID_DESKTOP)
    return profile


def switch_profile(link: Path, profile: Path) -> None:
    # Replace the symlink atomically, the way Nix and Guix switch profiles
    new_link = link.with_name(f"{link.name}.new")
    new_link.symlink_to(profile)
    new_link.replace(link)


@pytest.fixture
def xdg_data_home_files():
    return {
        "applications/org.example.CorrectAppId.desktop": CORRECT_APP_ID_DESKTOP,
    }


@pytest.fixture
def profiles(create_test_dirs) -> Path:
    profiles = Path(os.environ["HOME"]) / "profiles"
    profile = create_profile(profiles, "1", ["org.example.Old"])
    (profiles / "current").symlink_to(profile)
    return profiles


@pytest.fixture
def xdp_overwrite_env(profiles):
    data_dirs = os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share")
    return {"XDG_DATA_DIRS": f"{profiles / 'current' / 'share'}:{data_dirs}"}


@pytest.fixture
def xdp_app_info() -> xdp.AppInfo:
    return xdp.AppInfoHost(app_id="org.example.WrongAppId")


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}}


class TestRegistry:
    def test_version(self, portals, dbus_con):
        documents = dbus_con.get_object(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
        )

        properties_intf = dbus.Interface(
            documents,
            "org.freedesktop.DBus.Properties",
        )
        portal_version = properties_intf.Get(
            "org.freedesktop.host.portal.Registry",
            "version",
        )
        assert int(portal_version) == 1

    def create_dummy_session(self, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        request = xdp.Request(dbus_con, remotedesktop_intf)

        session_counter_attr_name = "session_counter"
        session_counter = getattr(self, session_counter_attr_name, 0)
        setattr(self, session_counter_attr_name, session_counter + 1)

        options = {
            "session_handle_token": f"session_token{session_counter}",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )
        assert response
        assert response.response == 0

        return xdp.Session.from_response(dbus_con, response)

    def test_registerless(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = app_id

        session = self.create_dummy_session(dbus_con)

        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

    def test_register(self, portals, dbus_con):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.CorrectAppId"
        registry_intf.Register(expected_app_id, {})

        session = self.create_dummy_session(dbus_con)

        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

    def test_late_register(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = app_id
        unexpected_app_id = "org.example.CorrectAppId"

        session = self.create_dummy_session(dbus_con)

        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

        with pytest.raises(dbus.exceptions.DBusException) as exc_info:
            registry_intf.Register(unexpected_app_id, {})
        exc_info.match(".*Connection already associated with an application ID.*")

        new_session = self.create_dummy_session(dbus_con)

        new_app_id = mock_intf.GetSessionAppId(new_session.handle)
        assert new_app_id == expected_app_id

    def test_multiple_connections(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.CorrectAppId"
        unexpected_app_id = app_id

        registry_intf.Register(expected_app_id, {})
        session = self.create_dummy_session(dbus_con)
        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

        dbus_con2 = dbus.bus.BusConnection(dbus.bus.BusConnection.TYPE_SESSION)
        dbus_con2.set_exit_on_disconnect(False)
        mock_intf2 = xdp.get_mock_iface(dbus_con2)
        session2 = self.create_dummy_session(dbus_con2)
        app_id2 = mock_intf2.GetSessionAppId(session2.handle)
        assert app_id2 == unexpected_app_id
        dbus_con2.close()

        dbus_con3 = dbus.bus.BusConnection(dbus.bus.BusConnection.TYPE_SESSION)
        dbus_con3.set_exit_on_disconnect(False)
        mock_intf3 = xdp.get_mock_iface(dbus_con3)
        registry_intf3 = xdp.get_portal_iface(dbus_con3, "Registry", domain="host")
        registry_intf3.Register(expected_app_id, {})
        session3 = self.create_dummy_session(dbus_con3)
        app_id3 = mock_intf3.GetSessionAppId(session3.handle)
        assert app_id3 == expected_app_id
        dbus_con3.close()

    def test_register_invalid_app_id(self, portals, dbus_con):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        with pytest.raises(dbus.exceptions.DBusException) as exc_info:
            registry_intf.Register("org.example.NonexistentApp", {})
        assert "Can't create registered app" in str(exc_info.value)

        session = self.create_dummy_session(dbus_con)
        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id

    def test_no_reregister(self, portals, dbus_con):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.CorrectAppId"

        registry_intf.Register(expected_app_id, {})
        session = self.create_dummy_session(dbus_con)
        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

        with pytest.raises(dbus.exceptions.DBusException) as exc_info:
            registry_intf.Register(expected_app_id, {})
        exc_info.match(".*Connection already associated with an application ID.*")

    def test_register_after_profile_switch(self, portals, dbus_con, profiles):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.New"

        # Let xdg-desktop-portal look for the desktop file before it exists
        with pytest.raises(dbus.exceptions.DBusException) as exc_info:
            registry_intf.Register(expected_app_id, {})
        assert "Can't create registered app" in str(exc_info.value)

        # Install the app by switching the profile. No directory a file
        # monitor could be watching changes.
        profile = create_profile(profiles, "2", ["org.example.Old", expected_app_id])
        switch_profile(profiles / "current", profile)

        registry_intf.Register(expected_app_id, {})

        session = self.create_dummy_session(dbus_con)
        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id
