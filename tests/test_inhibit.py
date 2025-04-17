# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
from enum import Enum, Flag


class InhibitFlags(Flag):
    LOGOUT = 1
    USER_SWITCH = 2
    SUSPEND = 4
    IDLE = 8
    ALL = 16 - 1


class SessionState(Enum):
    RUNNING = 1
    QUERY_END = 2
    ENDING = 3


@pytest.fixture
def required_templates():
    return {"inhibit": {}}


class TestInhibit:
    def set_permissions(self, dbus_con, app_id, permissions):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetPermission(
            "inhibit",
            True,
            "inhibit",
            app_id,
            permissions,
        )

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Inhibit", 3)

    def test_basic(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        inhibit_intf = xdp.get_portal_iface(dbus_con, "Inhibit")
        mock_intf = xdp.get_mock_iface(dbus_con)

        reason = "reason"
        flags = InhibitFlags.ALL

        request = xdp.Request(dbus_con, inhibit_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "Inhibit",
            window="",
            flags=flags.value,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Inhibit")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == flags.value
        assert args[4]["reason"] == reason

    @pytest.mark.parametrize(
        "token", ("Invalid-Token&", "", "/foo", "something-else", "😄")
    )
    def test_inhibit_invalid_handle_token(self, portals, dbus_con, token):
        inhibit_intf = xdp.get_portal_iface(dbus_con, "Inhibit")

        request = xdp.Request(dbus_con, inhibit_intf)
        options = {"handle_token": token}

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call("Inhibit", window="", flags=0, options=options)

        e = excinfo.value
        assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        assert "Invalid token" in e.get_dbus_message()

    @pytest.mark.parametrize("template_params", ({"inhibit": {"response": 1}},))
    def test_cancel(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        inhibit_intf = xdp.get_portal_iface(dbus_con, "Inhibit")
        mock_intf = xdp.get_mock_iface(dbus_con)

        reason = "reason"
        flags = InhibitFlags.ALL

        request = xdp.Request(dbus_con, inhibit_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "Inhibit",
            window="",
            flags=flags.value,
            options=options,
        )

        # for some reason, the backend failing is still considered a success
        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Inhibit")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == flags.value
        assert args[4]["reason"] == reason

    @pytest.mark.parametrize("template_params", ({"inhibit": {"expect-close": True}},))
    def test_close(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        inhibit_intf = xdp.get_portal_iface(dbus_con, "Inhibit")
        mock_intf = xdp.get_mock_iface(dbus_con)

        reason = "reason"
        flags = InhibitFlags.ALL

        request = xdp.Request(dbus_con, inhibit_intf)
        request.schedule_close(1000)
        options = {
            "reason": reason,
        }
        request.call(
            "Inhibit",
            window="",
            flags=flags.value,
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Inhibit")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == flags.value
        assert args[4]["reason"] == reason

    def test_permission(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        inhibit_intf = xdp.get_portal_iface(dbus_con, "Inhibit")
        mock_intf = xdp.get_mock_iface(dbus_con)

        self.set_permissions(dbus_con, app_id, ["logout", "suspend"])

        reason = "reason"
        flags = InhibitFlags.LOGOUT | InhibitFlags.SUSPEND | InhibitFlags.IDLE
        allowed_flags = InhibitFlags.LOGOUT | InhibitFlags.SUSPEND

        request = xdp.Request(dbus_con, inhibit_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "Inhibit",
            window="",
            flags=flags.value,
            options=options,
        )

        assert response
        assert response.response == 0

        method_calls = mock_intf.GetMethodCalls("Inhibit")
        _, args = method_calls[-1]
        assert args[3] == allowed_flags.value

        self.set_permissions(dbus_con, app_id, ["suspend"])

        flags = InhibitFlags.LOGOUT | InhibitFlags.SUSPEND | InhibitFlags.IDLE
        allowed_flags = InhibitFlags.SUSPEND

        request = xdp.Request(dbus_con, inhibit_intf)
        options = {
            "reason": reason,
        }
        response = request.call(
            "Inhibit",
            window="",
            flags=flags.value,
            options=options,
        )

        assert response
        assert response.response == 0

        method_calls = mock_intf.GetMethodCalls("Inhibit")
        _, args = method_calls[-1]
        assert args[3] == allowed_flags.value

    def test_monitor(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        inhibit_intf = xdp.get_portal_iface(dbus_con, "Inhibit")
        mock_intf = xdp.get_mock_iface(dbus_con)

        changed_count = 0

        request = xdp.Request(dbus_con, inhibit_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateMonitor",
            window="",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateMonitor")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == app_id
        assert args[3] == ""  # parent window

        def state_changed_cb(session_handle, state):
            nonlocal changed_count

            assert not state["screensaver-active"]
            assert state["session-state"] == SessionState.QUERY_END.value

            changed_count += 1

        inhibit_intf.connect_to_signal("StateChanged", state_changed_cb)

        # wait for a Query End state change
        xdp.wait_for(lambda: changed_count == 1)
        assert not session.closed
        # and respond with QueryEndResponse
        inhibit_intf.QueryEndResponse(session.handle)

        # wait for another Query End state change
        xdp.wait_for(lambda: changed_count == 2)
        assert not session.closed

        # do not respond with QueryEndResponse and instead wait for >1s
        xdp.wait(1500)

        # the session should have gotten closed by now
        assert session.closed
