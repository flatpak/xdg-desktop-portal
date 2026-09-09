# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import json
import os
import socket
from pathlib import Path

import pytest

import tests.xdp_utils as xdp

VARLINK_SOCKET = "org.freedesktop.portal"
REGISTRY = "org.freedesktop.host.portal.Registry"


def socket_path(socket_name: str = VARLINK_SOCKET) -> Path:
    return Path(os.environ["XDG_RUNTIME_DIR"]) / socket_name


class VarlinkConnection:
    """
    A connection to a portal varlink socket. Messages are NUL terminated JSON
    objects, and calls may be pipelined ahead of reading their replies.
    """

    def __init__(self, socket_name: str = VARLINK_SOCKET):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(10)
        self.socket.connect(os.fspath(socket_path(socket_name)))
        self._buffer = b""

    def send(self, method: str, more: bool = False, **parameters) -> None:
        message: dict = {"method": method}
        if parameters:
            message["parameters"] = parameters
        if more:
            message["more"] = True
        self.socket.sendall(json.dumps(message).encode() + b"\0")

    def receive(self) -> dict:
        while b"\0" not in self._buffer:
            data = self.socket.recv(4096)
            assert data, "Connection closed before the reply arrived"
            self._buffer += data

        reply, _, self._buffer = self._buffer.partition(b"\0")
        return json.loads(reply)

    def call(self, method: str, **parameters) -> dict:
        self.send(method, **parameters)
        return self.receive()

    def register(self, app_id_hint: str = "") -> str:
        reply = self.call(f"{REGISTRY}.Register", app_id_hint=app_id_hint)
        assert "error" not in reply, reply
        return reply["parameters"]["instance_id"]

    def close(self) -> None:
        self.socket.close()

    def __enter__(self) -> "VarlinkConnection":
        return self

    def __exit__(self, *args) -> None:
        self.close()


@pytest.fixture
def xdp_app_info() -> xdp.AppInfo:
    return xdp.AppInfoHost()


class TestVarlink:
    def test_socket_exists(self, portals):
        assert socket_path().is_socket()

    def test_get_info(self, portals):
        with VarlinkConnection() as conn:
            reply = conn.call("org.varlink.service.GetInfo")

        assert "error" not in reply, reply
        assert reply["parameters"]["product"] == "xdg-desktop-portal"
        assert REGISTRY in reply["parameters"]["interfaces"]


class TestVarlinkRegistry:
    def test_register(self, portals):
        with VarlinkConnection() as conn:
            assert conn.register()

    def test_register_twice(self, portals):
        with VarlinkConnection() as conn:
            conn.register()
            reply = conn.call(f"{REGISTRY}.Register", app_id_hint="")

        assert reply["error"] == f"{REGISTRY}.AlreadyRegistered"

    def test_register_missing_parameter(self, portals):
        with VarlinkConnection() as conn:
            reply = conn.call(f"{REGISTRY}.Register")

        assert reply["error"] == "org.varlink.service.InvalidParameter"

    def test_claim(self, portals):
        with VarlinkConnection() as first:
            instance_id = first.register()

            with VarlinkConnection() as second:
                reply = second.call(f"{REGISTRY}.Claim", instance_id=instance_id)
                assert "error" not in reply, reply

    def test_claim_unknown_instance(self, portals):
        with VarlinkConnection() as conn:
            reply = conn.call(f"{REGISTRY}.Claim", instance_id="nope")

        assert reply["error"] == f"{REGISTRY}.NoSuchInstance"

    def test_claim_pipelined(self, portals):
        """
        The expected client pattern: pipeline a Claim with the calls that
        depend on it, and fall back to Register when the instance is gone.
        """
        with VarlinkConnection() as conn:
            conn.send(f"{REGISTRY}.Claim", instance_id="nope")
            conn.send(f"{REGISTRY}.Register", app_id_hint="")

            assert conn.receive()["error"] == f"{REGISTRY}.NoSuchInstance"
            assert conn.receive()["parameters"]["instance_id"]

    def test_instance_outlives_its_creator(self, portals):
        first = VarlinkConnection()
        instance_id = first.register()

        second = VarlinkConnection()
        assert "error" not in second.call(f"{REGISTRY}.Claim", instance_id=instance_id)
        first.close()

        with VarlinkConnection() as third:
            reply = third.call(f"{REGISTRY}.Claim", instance_id=instance_id)
            assert "error" not in reply, reply

        second.close()

    def test_instance_dies_with_its_last_claim(self, portals):
        conn = VarlinkConnection()
        instance_id = conn.register()
        conn.close()

        with VarlinkConnection() as other:
            reply = other.call(f"{REGISTRY}.Claim", instance_id=instance_id)

        assert reply["error"] == f"{REGISTRY}.NoSuchInstance"


class TestVarlinkRegistrySandboxed:
    @pytest.fixture
    def xdp_app_info(self) -> xdp.AppInfo:
        return xdp.AppInfoFlatpak()

    def test_register_and_claim(self, portals):
        with VarlinkConnection() as conn:
            instance_id = conn.register(app_id_hint="org.example.Impostor")

            with VarlinkConnection() as other:
                reply = other.call(f"{REGISTRY}.Claim", instance_id=instance_id)
                assert "error" not in reply, reply
