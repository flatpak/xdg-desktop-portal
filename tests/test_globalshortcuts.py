# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from tests import Session
from gi.repository import GLib

import dbus
import pytest
import time


@pytest.fixture
def portal_name():
    return "GlobalShortcuts"


class TestGlobalShortcuts:
    def test_version(self, portal_mock):
        portal_mock.check_version(1)

    def test_global_shortcuts_create_close_session(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == ""  # appid

        session.close()

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    @pytest.mark.parametrize("params", ({"force-close": 500},))
    def test_global_shortcuts_create_session_signal_closed(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)
        # Check the impl portal was called with the right args
        method_calls = portal_mock.mock_interface.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == session.handle
        assert args[2] == ""  # appid

        # Now expect the backend to close it

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    def test_global_shortcuts_bind_list_shortcuts(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)

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

        request = portal_mock.create_request()
        response = request.call(
            "BindShortcuts",
            session_handle=session.handle,
            shortcuts=shortcuts,
            parent_window="",
            options={},
        )

        request = portal_mock.create_request()
        options = {}
        response = request.call(
            "ListShortcuts",
            session_handle=session.handle,
            options=options,
        )

        assert len(list(response.results["shortcuts"])) == len(list(shortcuts))

        session.close()

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed

    def test_global_shortcuts_trigger(self, portal_mock):
        request = portal_mock.create_request()
        options = {
            "session_handle_token": "session_token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )

        assert response.response == 0

        session = Session.from_response(portal_mock.dbus_con, response)

        shortcuts = [
            (
                "binding1",
                {
                    "description": dbus.String("Binding #1", variant_level=1),
                    "preferred-trigger": dbus.String("CTRL+a", variant_level=1),
                },
            ),
        ]

        request = portal_mock.create_request()
        response = request.call(
            "BindShortcuts",
            session_handle=session.handle,
            shortcuts=shortcuts,
            parent_window="",
            options={},
        )

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

        gs_intf = portal_mock.get_dbus_interface()
        gs_intf.connect_to_signal("Activated", cb_activated)
        gs_intf.connect_to_signal("Deactivated", cb_deactivated)

        portal_mock.mock_interface.Trigger(session.handle, "binding1")

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert activated_count == 1
        assert deactivated_count == 1

        session.close()

        mainloop = GLib.MainLoop()
        GLib.timeout_add(2000, mainloop.quit)
        mainloop.run()

        assert session.closed
