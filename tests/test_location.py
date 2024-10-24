# SPDX-License-Identifier: LGPL-2.1-or-later


from tests import Session
from gi.repository import GLib

import pytest
import dbus


@pytest.fixture
def portal_name():
    return "Location"


@pytest.fixture
def portal_has_impl():
    return False


@pytest.fixture
def required_templates():
    return {"geoclue2": {}}


class TestLocation:
    def test_version(self, portal_mock):
        portal_mock.check_version(1)


    def get_client_mock(self, portal_mock):
        geoclue_manager_proxy = portal_mock.dbus_con_sys.get_object(
            "org.freedesktop.GeoClue2",
            "/org/freedesktop/GeoClue2/Manager",
        )
        geoclue_manager = dbus.Interface(
            geoclue_manager_proxy,
            "org.freedesktop.GeoClue2.Manager"
        )
        geoclue_client_proxy = portal_mock.dbus_con_sys.get_object(
            "org.freedesktop.GeoClue2",
            geoclue_manager.GetClient()
        )
        client_mock = dbus.Interface(
            geoclue_client_proxy,
            "org.freedesktop.GeoClue2.Mock"
        )
        return client_mock


    def test_session_update(self, portal_mock):
        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        updated_count = 0

        location_intf = portal_mock.get_dbus_interface()
        session = Session(
            portal_mock.dbus_con,
            location_intf.CreateSession({"session_handle_token": "session_token0"})
        )

        def cb_location_updated(session_handle, location):
            nonlocal mainloop
            nonlocal updated_count

            if updated_count == 0:
                assert location["Latitude"] == 0
                assert location["Longitude"] == 0
                assert location["Accuracy"] == 0
            elif updated_count == 1:
                assert location["Latitude"] == 11
                assert location["Longitude"] == 22
                assert location["Accuracy"] == 3

            updated_count += 1
            mainloop.quit()

        location_intf.connect_to_signal("LocationUpdated", cb_location_updated)

        start_session_request = portal_mock.create_request()
        start_session_response = start_session_request.call(
            "Start",
            session_handle=session.handle,
            parent_window="window-hndl",
            options = {},
        )

        assert start_session_response.response == 0

        mainloop.run()

        assert updated_count == 1

        client_mock = self.get_client_mock(portal_mock)
        client_mock.ChangeLocation({
            "Latitude": dbus.UInt32(11),
            "Longitude": dbus.UInt32(22),
            "Accuracy": dbus.UInt32(3),
        })

        mainloop.run()

        assert updated_count == 2


    def test_bad_accuracy(self, portal_mock):
        had_error = False
        location_intf = portal_mock.get_dbus_interface()
        try:
            location_intf.CreateSession({
                "session_handle_token": "session_token0",
                "accuracy": dbus.UInt32(22),
            })
        except dbus.exceptions.DBusException as e:
            had_error = True
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        finally:
            assert had_error

