# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import os
import socket
from typing import Any

import dbus
import dbusmock
import pytest

import tests.xdp_utils as xdp


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}}


class TestRemoteDesktop:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "RemoteDesktop", 2)

    def test_create_close_session(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, remotedesktop_intf)
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
        # assert args[2] == ""  # appid, not necessary empty

        session.close()
        xdp.wait_for(lambda: session.closed)

    @pytest.mark.parametrize(
        "token", ("Invalid-Token&", "", "/foo", "something-else", "😄")
    )
    def test_remote_desktop_create_session_invalid(self, portals, dbus_con, token):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {"session_handle_token": token}

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call("CreateSession", options=options)

        e = excinfo.value
        assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
        assert "Invalid token" in e.get_dbus_message()

    @pytest.mark.parametrize(
        "template_params", ({"remotedesktop": {"force-close": 500}},)
    )
    def test_create_session_signal_closed(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, remotedesktop_intf)
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
        # assert args[2] == ""  # appid, not necessary empty

        # Now expect the backend to close it
        xdp.wait_for(lambda: session.closed)

    @pytest.mark.parametrize("replacement", (False, True))
    def test_backend_owner_loss_closes_sessions(
        self, portals, dbus_con, templates, xdg_desktop_portal, replacement
    ):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        backend_name = "org.freedesktop.impl.portal.Test"
        sessions = []
        for index in range(2):
            request = xdp.Request(dbus_con, remotedesktop_intf)
            response = request.call(
                "CreateSession",
                options={"session_handle_token": f"backend_loss{index}"},
            )
            assert response and response.response == 0
            sessions.append(xdp.Session.from_response(dbus_con, response))

        replacement_con = None
        try:
            if replacement:
                # Queue the next owner so there is no intermediate unowned
                # state when the backend disconnects.
                replacement_con = dbus.bus.BusConnection(
                    os.environ["DBUS_SESSION_BUS_ADDRESS"]
                )
                assert (
                    replacement_con.request_name(backend_name, 0)
                    == dbus.bus.REQUEST_NAME_REPLY_IN_QUEUE
                )

            backend = templates[dbusmock.BusType.SESSION][backend_name]
            backend.process.kill()
            backend.process.wait(timeout=5)
            xdp.wait_for(lambda: all(session.closed for session in sessions))
            assert all(session.error is None for session in sessions)

            if replacement_con:
                assert (
                    dbus_con.get_name_owner(backend_name)
                    == replacement_con.get_unique_name()
                )
            else:
                assert not dbus_con.name_has_owner(backend_name)

            xdp.wait(50)
            assert xdg_desktop_portal.poll() is None
            for session in sessions:
                request = xdp.Request(dbus_con, remotedesktop_intf)
                with pytest.raises(dbus.exceptions.DBusException) as excinfo:
                    request.call(
                        "SelectDevices",
                        session_handle=session.handle,
                        options={"types": dbus.UInt32(3)},
                    )
                assert (
                    excinfo.value.get_dbus_name()
                    == "org.freedesktop.DBus.Error.AccessDenied"
                )
        finally:
            if replacement_con:
                replacement_con.close()

    def test_connect_to_eis(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
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
        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response
        assert response.response == 0

        fd = remotedesktop_intf.ConnectToEIS(
            session.handle,
            dbus.Dictionary({}, signature="sv"),
        )
        eis_socket = socket.fromfd(fd.take(), socket.AF_UNIX, socket.SOCK_STREAM)
        assert eis_socket.recv(10) == b"HELLO"

    @pytest.mark.parametrize(
        "template_params", ({"remotedesktop": {"fail-connect-to-eis": True}},)
    )
    def test_connect_to_eis_fail(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
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
        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response
        assert response.response == 0

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            _ = remotedesktop_intf.ConnectToEIS(
                session.handle, dbus.Dictionary({}, signature="sv")
            )
        assert "Purposely failing ConnectToEIS" in excinfo.value.get_dbus_message()

    def test_connect_to_eis_fail_notifies(self, portals, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")

        request = xdp.Request(dbus_con, remotedesktop_intf)
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
        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {
            "types": dbus.UInt32(0x3),
        }
        response = request.call(
            "SelectDevices",
            session_handle=session.handle,
            options=options,
        )
        assert response
        assert response.response == 0

        request = xdp.Request(dbus_con, remotedesktop_intf)
        options = {}
        response = request.call(
            "Start",
            session_handle=session.handle,
            parent_window="",
            options=options,
        )
        assert response
        assert response.response == 0

        notifyfuncs: list[dict[str, Any]] = [
            {"name": "NotifyPointerMotion", "args": (1, 2)},
            {"name": "NotifyPointerMotionAbsolute", "args": (0, 1, 2)},
            {"name": "NotifyPointerButton", "args": (1, 1)},
            {"name": "NotifyPointerAxis", "args": (1, 1)},
            {"name": "NotifyPointerAxisDiscrete", "args": (1, 1)},
            {"name": "NotifyKeyboardKeycode", "args": (1, 1)},
            {"name": "NotifyKeyboardKeysym", "args": (1, 1)},
            {"name": "NotifyTouchDown", "args": (0, 0, 1, 1)},
            {"name": "NotifyTouchMotion", "args": (0, 0, 1, 1)},
            {"name": "NotifyTouchUp", "args": (0,)},
        ]
        for notifyfunc in notifyfuncs:
            with pytest.raises(dbus.exceptions.DBusException) as excinfo:
                func = getattr(remotedesktop_intf, notifyfunc["name"])
                assert func is not None
                func(
                    session.handle,
                    dbus.Dictionary({}, signature="sv"),
                    *notifyfunc["args"],
                )
            # Not the best error message but...
            assert (
                "Session is not allowed to call Notify"
                in excinfo.value.get_dbus_message()
            )
