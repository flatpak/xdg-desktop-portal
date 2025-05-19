# SPDX-License-Identifier: LGPL-2.1-or-later

import tests.xdp_utils as xdp

import pytest
import dbus


@pytest.fixture
def required_templates():
    return {"geoclue2": {}}


class TestLocation:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Location", 1)

    def get_geoclue_mock(self, dbus_con_sys):
        geoclue_manager_proxy = dbus_con_sys.get_object(
            "org.freedesktop.GeoClue2",
            "/org/freedesktop/GeoClue2/Manager",
        )
        geoclue_manager = dbus.Interface(
            geoclue_manager_proxy, "org.freedesktop.GeoClue2.Manager"
        )
        geoclue_client_proxy = dbus_con_sys.get_object(
            "org.freedesktop.GeoClue2", geoclue_manager.GetClient()
        )
        geoclue_mock = dbus.Interface(
            geoclue_client_proxy, "org.freedesktop.GeoClue2.Mock"
        )
        return geoclue_mock

    @pytest.mark.parametrize("required_templates", ({},))
    def test_no_geoclue(self, portals, dbus_con):
        location_intf = xdp.get_portal_iface(dbus_con, "Location")

        session = xdp.Session(
            dbus_con,
            location_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        start_session_request = xdp.Request(dbus_con, location_intf)
        start_session_response = start_session_request.call(
            "Start",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )

        assert start_session_response
        assert start_session_response.response == 2

    def test_session_update(self, portals, dbus_con, dbus_con_sys):
        location_intf = xdp.get_portal_iface(dbus_con, "Location")
        geoclue_mock_intf = self.get_geoclue_mock(dbus_con_sys)

        location_updated = False
        updated_count = 0

        session = xdp.Session(
            dbus_con,
            location_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        def cb_location_updated(session_handle, location):
            nonlocal location_updated
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
            location_updated = True

        location_intf.connect_to_signal("LocationUpdated", cb_location_updated)

        start_session_request = xdp.Request(dbus_con, location_intf)
        start_session_response = start_session_request.call(
            "Start",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )

        assert start_session_response
        assert start_session_response.response == 0

        xdp.wait_for(lambda: location_updated)
        location_updated = False

        assert updated_count == 1

        geoclue_mock_intf.ChangeLocation(
            {
                "Latitude": dbus.UInt32(11),
                "Longitude": dbus.UInt32(22),
                "Accuracy": dbus.UInt32(3),
            }
        )

        xdp.wait_for(lambda: location_updated)
        location_updated = False

        assert updated_count == 2

    def test_bad_accuracy(self, portals, dbus_con):
        location_intf = xdp.get_portal_iface(dbus_con, "Location")
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            location_intf.CreateSession(
                {
                    "session_handle_token": "session_token0",
                    "accuracy": dbus.UInt32(22),
                }
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )
