# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
import time


@pytest.fixture
def required_templates():
    return {"globalshortcuts": {}}


class TestGlobalShortcuts:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "GlobalShortcuts", 2)

    def test_create_close_session(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == app_id

        session.close()
        xdp.wait_for(lambda: session.closed)

    @pytest.mark.parametrize(
        "template_params", ({"globalshortcuts": {"force-close": 500}},)
    )
    def test_create_session_signal_closed(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == app_id

        # Now expect the backend to close it
        xdp.wait_for(lambda: session.closed)

    def test_bind_list_shortcuts(self, portals, dbus_con):
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)

        shortcuts = [
            (
                "binding1",
                {
                    "description": dbus.String("Binding #1", variant_level=1),
                    "preferred-trigger": dbus.String("CTRL+a", variant_level=1),
                },
            ),
            (
                "binding2",
                {
                    "description": dbus.String("Binding #2", variant_level=1),
                    "preferred-trigger": dbus.String("CTRL+b", variant_level=1),
                },
            ),
        ]

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        response = request.call(
            "BindShortcuts",
            session_handle=session.handle,
            shortcuts=shortcuts,
            parent_window="",
            options={},
        )

        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {}
        response = request.call(
            "ListShortcuts",
            session_handle=session.handle,
            options=options,
        )

        assert response
        assert response.response == 0

        assert len(list(response.results["shortcuts"])) == len(list(shortcuts))

        session.close()
        xdp.wait_for(lambda: session.closed)

    def test_bind_no_shortcuts(self, portals, dbus_con):
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        response = request.call(
            "BindShortcuts",
            session_handle=session.handle,
            shortcuts=[],
            parent_window="",
            options={},
        )

        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {}
        response = request.call(
            "ListShortcuts",
            session_handle=session.handle,
            options=options,
        )

        assert response
        assert response.response == 0

        assert len(list(response.results["shortcuts"])) == 0

        session.close()
        xdp.wait_for(lambda: session.closed)

    def test_trigger(self, portals, dbus_con):
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)

        shortcuts = [
            (
                "binding1",
                {
                    "description": dbus.String("Binding #1", variant_level=1),
                    "preferred-trigger": dbus.String("CTRL+a", variant_level=1),
                },
            ),
        ]

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        response = request.call(
            "BindShortcuts",
            session_handle=session.handle,
            shortcuts=shortcuts,
            parent_window="",
            options={},
        )

        assert response
        assert response.response == 0

        activated_count = 0
        deactivated_count = 0

        def cb_activated(session_handle, shortcut_id, timestamp, options):
            nonlocal activated_count
            now_since_epoch = int(time.time() * 1000000)
            # This assert will race twice a year on systems configured with
            # summer time timezone changes
            assert (
                now_since_epoch > timestamp
                and (now_since_epoch - 10 * 10001000) < timestamp
            )
            assert shortcut_id == "binding1"
            activated_count += 1

        def cb_deactivated(session_handle, shortcut_id, timestamp, options):
            nonlocal deactivated_count
            now_since_epoch = int(time.time() * 1000000)
            # This assert will race twice a year on systems configured with
            # summer time timezone changes
            assert (
                now_since_epoch > timestamp
                and (now_since_epoch - 10 * 10001000) < timestamp
            )
            assert shortcut_id == "binding1"
            deactivated_count += 1

        globalshortcuts_intf.connect_to_signal("Activated", cb_activated)
        globalshortcuts_intf.connect_to_signal("Deactivated", cb_deactivated)

        mock_intf.Trigger(session.handle, "binding1")

        xdp.wait_for(lambda: activated_count == 1 and deactivated_count == 1)
        assert not session.closed

        session.close()
        xdp.wait_for(lambda: session.closed)

    def test_configure_shortcuts(self, portals, dbus_con):
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        parent_window = ""
        options = {"activation_token": "token_123"}

        globalshortcuts_intf.ConfigureShortcuts(session.handle, parent_window, options)

        method_calls = mock_intf.GetMethodCalls("ConfigureShortcuts")
        assert len(method_calls) > 0
        _, args = method_calls[-1]

        assert args[0] == session.handle
        assert args[1] == ""
        assert args[2] == options
