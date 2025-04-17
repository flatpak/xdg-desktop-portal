# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest


ACCOUNT_DATA = {
    "id": "test",
    "name": "Test Name",
    "image": "file:///image.png",
}


@pytest.fixture
def required_templates():
    return {
        "account": {
            "results": ACCOUNT_DATA,
        },
    }


class TestAccount:
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
        xdp.check_version(dbus_con, "Account", 1)

    def test_basic1(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        account_intf = xdp.get_portal_iface(dbus_con, "Account")
        mock_intf = xdp.get_mock_iface(dbus_con)

        reason = "reason"

        request = xdp.Request(dbus_con, account_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "GetUserInformation",
            window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["id"] == ACCOUNT_DATA["id"]
        assert response.results["name"] == ACCOUNT_DATA["name"]
        assert response.results["image"] == ACCOUNT_DATA["image"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("GetUserInformation")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # window
        assert args[3]["reason"] == reason

    def test_reason(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")
        mock_intf = xdp.get_mock_iface(dbus_con)

        reason = """This reason is unreasonably long, it stretches over
                more than twohundredfiftysix characters, which is really quite
                long. Excessively so. The portal frontend will silently drop
                reasons of this magnitude. If you can't express your reasons
                concisely, you probably have no good reason in the first place
                and are just waffling around."""

        assert len(reason) - 1 > 256

        request = xdp.Request(dbus_con, account_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "GetUserInformation",
            window="",
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("GetUserInformation")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert "reason" not in args[3]

    @pytest.mark.parametrize("template_params", ({"account": {"expect-close": True}},))
    def test_close(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")

        reason = "reason"

        request = xdp.Request(dbus_con, account_intf)
        request.schedule_close(1000)
        options = {
            "reason": reason,
        }
        request.call(
            "GetUserInformation",
            window="",
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed
