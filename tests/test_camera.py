# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest


@pytest.fixture
def required_templates():
    return {
        "access": {},
        "lockdown": {},
    }


class TestCamera:
    def set_permissions(self, dbus_con, appid, permissions):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetPermission(
            "devices",
            True,
            "camera",
            appid,
            permissions,
        )

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Camera", 1)

    def test_access(self, portals, dbus_con, app_id):
        camera_intf = xdp.get_portal_iface(dbus_con, "Camera")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, camera_intf)
        response = request.call(
            "AccessCamera",
            options={},
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AccessDialog")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[1] == app_id

    @pytest.mark.parametrize("template_params", ({"access": {"response": 1}},))
    def test_access_cancel(self, portals, dbus_con, app_id):
        camera_intf = xdp.get_portal_iface(dbus_con, "Camera")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, camera_intf)
        response = request.call(
            "AccessCamera",
            options={},
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AccessDialog")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[1] == app_id

    @pytest.mark.parametrize("template_params", ({"access": {"expect-close": True}},))
    def test_access_close(self, portals, dbus_con, app_id):
        camera_intf = xdp.get_portal_iface(dbus_con, "Camera")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, camera_intf)
        request.schedule_close(1000)
        request.call(
            "AccessCamera",
            options={},
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AccessDialog")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[1] == app_id

    @pytest.mark.parametrize(
        "template_params", ({"lockdown": {"disable-camera": True}},)
    )
    def test_access_lockdown(self, portals, dbus_con, app_id):
        camera_intf = xdp.get_portal_iface(dbus_con, "Camera")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, camera_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "AccessCamera",
                options={},
            )
        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AccessDialog")
        assert len(method_calls) == 0

    def test_access_denied(self, portals, dbus_con, app_id):
        camera_intf = xdp.get_portal_iface(dbus_con, "Camera")
        mock_intf = xdp.get_mock_iface(dbus_con)

        self.set_permissions(dbus_con, app_id, ["no"])

        request = xdp.Request(dbus_con, camera_intf)
        response = request.call(
            "AccessCamera",
            options={},
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AccessDialog")
        assert len(method_calls) == 0
