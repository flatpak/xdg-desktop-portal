# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import os

import pytest
from gi.repository import Gio, GLib

import tests.xdp_utils as xdp


@pytest.fixture
def required_templates():
    return {"globalshortcuts": {}}


class TestSession:
    def test_close(self, portals, dbus_con, xdp_app_info):
        globalshortcuts_intf = xdp.get_portal_iface(dbus_con, "GlobalShortcuts")

        request = xdp.Request(dbus_con, globalshortcuts_intf)
        response = request.call(
            "CreateSession",
            options={"session_handle_token": "t0"},
        )
        assert response
        assert response.response == 0

        session = xdp.Session.from_response(dbus_con, response)
        session.close()
        xdp.wait_for(lambda: session.closed)
        assert session.closed

    def test_disconnect(self, portals, dbus_con, xdp_app_info):
        impl_closed = [False]

        def on_session_closed(handle):
            impl_closed[0] = True

        dbus_con.add_signal_receiver(
            on_session_closed,
            "SessionClosed",
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

        result = con.call_sync(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.GlobalShortcuts",
            "CreateSession",
            GLib.Variant(
                "(a{sv})",
                ({"session_handle_token": GLib.Variant("s", "t0")},),
            ),
            GLib.VariantType.new("(o)"),
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )
        request_handle = result.unpack()[0]

        response_received = [False]

        def on_response(
            connection,
            sender,
            object_path,
            interface_name,
            signal_name,
            parameters,
            user_data,
        ):
            response_received[0] = True

        con.signal_subscribe(
            None,
            "org.freedesktop.portal.Request",
            "Response",
            request_handle,
            None,
            Gio.DBusSignalFlags.NONE,
            on_response,
            None,
        )

        xdp.wait_for(lambda: response_received[0])

        con.close_sync(None)

        xdp.wait_for(lambda: impl_closed[0])
        assert impl_closed[0]
