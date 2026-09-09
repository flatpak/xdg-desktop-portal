# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import os

import pytest

import tests.xdp_utils as xdp
from tests.test_varlink import VarlinkConnection

SECRET = "org.freedesktop.portal.Secret"


@pytest.fixture
def required_templates():
    return {"secret": {}}


class TestVarlinkSecret:
    @pytest.fixture
    def xdp_app_info(self) -> xdp.AppInfo:
        return xdp.AppInfoHost()

    def retrieve(self, conn, token=None, read=True):
        """
        Retrieves the secret over a fresh pipe and returns (reply, secret).
        Pass read=False when the call is expected to fail, since a backend that
        refuses may not write anything.
        """
        read_fd, write_fd = os.pipe()

        try:
            parameters = {"fd_idx": 0}
            if token is not None:
                parameters["token"] = token

            reply = conn.call(
                f"{SECRET}.RetrieveSecret", fds=[write_fd], **parameters
            )
        finally:
            os.close(write_fd)

        if not read:
            os.close(read_fd)
            return reply, b""

        with os.fdopen(read_fd, "rb") as secret:
            return reply, secret.read()

    def test_retrieve_secret(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply, secret = self.retrieve(conn)
            assert "error" not in reply, reply
            assert secret == b"a-master-secret"
            assert reply["parameters"]["token"] == "a-token"

    def test_retrieve_secret_with_token(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply, secret = self.retrieve(conn, token="a-previous-token")
            assert "error" not in reply, reply
            assert secret == b"a-master-secret"

    def test_before_registering(self, portals):
        with VarlinkConnection() as conn:
            read_fd, write_fd = os.pipe()

            try:
                reply = conn.call(
                    f"{SECRET}.RetrieveSecret", fds=[write_fd], fd_idx=0
                )
            finally:
                os.close(write_fd)
                os.close(read_fd)

            assert reply["error"] == f"{SECRET}.NotRegistered"

    def test_missing_fd(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply = conn.call(f"{SECRET}.RetrieveSecret", fd_idx=0)
            assert reply["error"] == "org.varlink.service.InvalidParameter"

    @pytest.mark.parametrize(
        "template_params", ({"secret": {"response": 1}},)
    )
    def test_cancelled(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply, _ = self.retrieve(conn, read=False)
            assert reply["error"] == f"{SECRET}.Cancelled"

    @pytest.mark.parametrize(
        "template_params", ({"secret": {"response": 2}},)
    )
    def test_no_secret(self, portals):
        with VarlinkConnection() as conn:
            conn.register()

            reply, _ = self.retrieve(conn, read=False)
            assert reply["error"] == f"{SECRET}.NoSecret"
