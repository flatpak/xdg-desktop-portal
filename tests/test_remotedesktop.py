# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import socket
import time
from typing import Any

import dbus
import pytest

import tests.xdp_utils as xdp


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}}


def _wait_for(predicate, message):
    deadline = time.monotonic() + (
        xdp.DBUS_TIMEOUT * (10 if xdp.is_valgrind() else 1) / 1000
    )
    xdp.wait_for(lambda: predicate() or time.monotonic() >= deadline)
    assert predicate(), message


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

    @pytest.mark.parametrize(
        "replacement", (False, True), ids=("disconnect", "replacement")
    )
    def test_backend_owner_loss_closes_sessions(
        self, portals, dbus_con, backend_owner_loss, xdg_desktop_portal, replacement
    ):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        sessions = []
        closed_signals: dict[str, list[dict[str, Any]]] = {}
        for index in range(2):
            request = xdp.Request(dbus_con, remotedesktop_intf)
            response = request.call(
                "CreateSession",
                options={"session_handle_token": f"backend_loss{index}"},
            )
            assert response and response.response == 0
            session = xdp.Session.from_response(dbus_con, response)
            sessions.append(session)
            closed_signals[session.handle] = []
            session.session_interface.connect_to_signal(
                "Closed",
                lambda details, handle=session.handle: closed_signals[handle].append(
                    dict(details)
                ),
            )

        replacement_calls = backend_owner_loss(
            "org.freedesktop.impl.portal.Test", replacement
        )
        _wait_for(
            lambda: all(session.closed for session in sessions),
            "Backend owner loss did not close every public session",
        )
        assert all(session.error is None for session in sessions)

        for session in sessions:
            with pytest.raises(dbus.exceptions.DBusException) as excinfo:
                session.session_interface.Close()
            assert (
                excinfo.value.get_dbus_name()
                == "org.freedesktop.DBus.Error.UnknownMethod"
            )

            # A new backend owner must not revive sessions from the old one.
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

        xdp.wait(50)
        assert all(len(signals) == 1 for signals in closed_signals.values())
        assert not replacement_calls, (
            "The replacement backend received calls for the old owner's sessions"
        )
        assert xdg_desktop_portal.poll() is None

    @pytest.mark.parametrize(
        "template_params",
        ({"remotedesktop": {"expect-close": True}},),
        ids=("pending",),
    )
    @pytest.mark.parametrize(
        "replacement", (False, True), ids=("disconnect", "replacement")
    )
    def test_backend_owner_loss_completes_requests(
        self, portals, dbus_con, backend_owner_loss, xdg_desktop_portal, replacement
    ):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        mock_intf = xdp.get_mock_iface(dbus_con)
        requests = []
        responses: dict[str, list[tuple[int, dict[str, Any]]]] = {}
        replies: list[str] = []
        errors: list[Exception] = []
        for index in range(2):
            request = xdp.Request(dbus_con, remotedesktop_intf)
            requests.append(request)
            responses[request.handle] = []
            request.request_interface.connect_to_signal(
                "Response",
                lambda response, results, handle=request.handle: responses[
                    handle
                ].append((int(response), dict(results))),
            )
            # Do not use Request.call(), which waits for Response. The mock's
            # expect-close mode keeps both backend calls pending indefinitely.
            remotedesktop_intf.CreateSession(
                {
                    "handle_token": request.handle_token,
                    "session_handle_token": f"backend_pending{index}",
                },
                reply_handler=replies.append,
                error_handler=errors.append,
            )

        _wait_for(
            lambda: (
                errors
                or (
                    len(replies) == len(requests)
                    and len(mock_intf.GetMethodCalls("CreateSession")) == len(requests)
                )
            ),
            "The backend did not receive every pending CreateSession request",
        )
        assert not errors
        assert set(replies) == {request.handle for request in requests}
        assert not any(responses.values())

        replacement_calls = backend_owner_loss(
            "org.freedesktop.impl.portal.Test", replacement
        )
        _wait_for(
            lambda: all(responses.values()),
            "Backend owner loss did not complete every public request",
        )
        for request in requests:
            with pytest.raises(dbus.exceptions.DBusException) as excinfo:
                request.request_interface.Close()
            assert (
                excinfo.value.get_dbus_name()
                == "org.freedesktop.DBus.Error.UnknownMethod"
            )

        # Dispatch queued method failures and signals before checking that each
        # request was completed exactly once, including the disconnect race.
        xdp.wait(50)
        assert all(signals == [(2, {})] for signals in responses.values())
        assert not replacement_calls, (
            "The replacement backend received calls for the old owner's requests"
        )
        assert xdg_desktop_portal.poll() is None

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
