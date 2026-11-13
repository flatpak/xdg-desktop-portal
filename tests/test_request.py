# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import os
import time

import dbus
import dbusmock
import pytest
from gi.repository import Gio, GLib

import tests.xdp_utils as xdp

ACCOUNT_RESULTS = {
    "id": "test",
    "name": "Test Name",
}


@pytest.fixture
def required_templates():
    return {
        "account": {
            "results": ACCOUNT_RESULTS,
        },
    }


class TestRequest:
    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {},
                id="v2",
            ),
            pytest.param(
                {"account": {"results": ACCOUNT_RESULTS, "request-version": 1}},
                id="v1-polling-fallback",
            ),
        ),
    )
    def test_response(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")

        request = xdp.Request(dbus_con, account_intf)
        options = {
            "reason": "test",
        }
        response = request.call(
            "GetUserInformation",
            window="",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["id"] == ACCOUNT_RESULTS["id"]
        assert response.results["name"] == ACCOUNT_RESULTS["name"]

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {"account": {"expect-close": True}},
                id="v2",
            ),
            pytest.param(
                {"account": {"expect-close": True, "request-version": 1}},
                id="v1-polling-fallback",
            ),
        ),
    )
    def test_close(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")

        request = xdp.Request(dbus_con, account_intf)
        request.schedule_close(1000)
        options = {
            "reason": "test",
        }
        request.call(
            "GetUserInformation",
            window="",
            options=options,
        )

        assert request.closed

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {"account": {"expect-close": True}},
                id="v2",
            ),
            pytest.param(
                {"account": {"expect-close": True, "request-version": 1}},
                id="v1-polling-fallback",
            ),
        ),
    )
    def test_disconnect(self, portals, dbus_con):
        impl_closed = [False]

        def on_request_closed(handle):
            impl_closed[0] = True

        dbus_con.add_signal_receiver(
            on_request_closed,
            "RequestClosed",
            dbus_interface="org.freedesktop.impl.portal.Mock",
        )

        # Use Gio.DBusConnection instead of dbus.SessionBus(private=True)
        # because dbus-python's close() breaks signal delivery on other
        # connections sharing the same GLib main loop.
        address = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        con = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None,
            None,
        )

        con.call_sync(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Account",
            "GetUserInformation",
            GLib.Variant("(sa{sv})", ("", {"reason": GLib.Variant("s", "test")})),
            GLib.VariantType.new("(o)"),
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )

        con.close_sync(None)

        xdp.wait_for(lambda: impl_closed[0])
        assert impl_closed[0]

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {"account": {"results": ACCOUNT_RESULTS, "delay": 0}},
                id="v2",
            ),
            pytest.param(
                {
                    "account": {
                        "results": ACCOUNT_RESULTS,
                        "delay": 0,
                        "request-version": 1,
                    }
                },
                id="v1",
            ),
        ),
    )
    def test_fast_backend(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")
        request = xdp.Request(dbus_con, account_intf)
        start = time.monotonic()
        response = request.call(
            "GetUserInformation",
            window="",
            options={"reason": "test"},
        )
        elapsed = time.monotonic() - start

        assert response
        assert response.response == 0
        assert elapsed < 5

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {"account": {"results": ACCOUNT_RESULTS, "fail": True}},
                id="v2",
            ),
            pytest.param(
                {
                    "account": {
                        "results": ACCOUNT_RESULTS,
                        "fail": True,
                        "request-version": 1,
                    }
                },
                id="v1",
            ),
        ),
    )
    def test_failing_backend(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")
        request = xdp.Request(dbus_con, account_intf)
        start = time.monotonic()
        response = request.call(
            "GetUserInformation",
            window="",
            options={"reason": "test"},
        )
        elapsed = time.monotonic() - start

        assert response
        assert response.response == 2
        assert elapsed < 5

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {"account": {"expect-close": True, "request-export-delay": 1000}},
                id="v2",
            ),
            pytest.param(
                {
                    "account": {
                        "expect-close": True,
                        "request-export-delay": 1000,
                        "request-version": 1,
                    }
                },
                id="v1",
            ),
        ),
    )
    def test_close_right_after_handle(self, portals, dbus_con):
        account_intf = xdp.get_portal_iface(dbus_con, "Account")
        request = xdp.Request(dbus_con, account_intf)
        handle = account_intf.GetUserInformation(
            "", {"handle_token": request.handle_token, "reason": "test"}
        )
        assert handle == request.handle
        request.close()
        xdp.wait_for(lambda: request.closed)
        assert request.closed

    @pytest.mark.parametrize(
        "template_params",
        (
            pytest.param(
                {"account": {"expect-close": True, "request-export-delay": 1000}},
                id="v2-delayed-export",
            ),
            pytest.param(
                {
                    "account": {
                        "expect-close": True,
                        "request-version": 1,
                        "request-export-delay": 1000,
                    }
                },
                id="v1-delayed-export",
            ),
        ),
    )
    def test_disconnect_before_impl_request(self, portals, dbus_con):
        """The app disconnects after the frontend called the backend,
        but before the backend created its request."""
        impl_closed = [False]

        def on_request_closed(handle):
            impl_closed[0] = True

        dbus_con.add_signal_receiver(
            on_request_closed,
            "RequestClosed",
            dbus_interface="org.freedesktop.impl.portal.Mock",
        )

        address = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        con = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None,
            None,
        )

        backend_called = [False]

        def on_method_called(method_name, args):
            if method_name == "GetUserInformation":
                backend_called[0] = True

        mock_obj = dbus_con.get_object(
            "org.freedesktop.impl.portal.Test",
            "/org/freedesktop/portal/desktop",
        )
        dbus.Interface(mock_obj, dbusmock.MOCK_IFACE).connect_to_signal(
            "MethodCalled", on_method_called
        )

        con.call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Account",
            "GetUserInformation",
            GLib.Variant("(sa{sv})", ("", {"reason": GLib.Variant("s", "test")})),
            GLib.VariantType.new("(o)"),
            Gio.DBusCallFlags.NONE,
            -1,
            None,
            None,
        )
        con.flush_sync(None)
        xdp.wait_for(lambda: backend_called[0])
        con.close_sync(None)

        xdp.wait_for(lambda: impl_closed[0])
        assert impl_closed[0]
