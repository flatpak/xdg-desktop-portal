# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import dbus
import pytest

import tests.xdp_utils as xdp

HANDLER_BUS_NAME = "xyz.iinuwa.credentialsd.Credentials"
ORIGIN = "https://example.com"
CREATE_PUBLIC_KEY_OPTIONS = """
    {
      "challenge": "jdrE6w1jAk2D1eJttNdOb9jTP5788BWlPyhA4ZqCDNdrRS1yaqU-DBD_H6rKaGe1t-2xoYNLYbUVKiOEVmaxNg",
      "rp": {
        "name": "example.com",
        "id": "example.com"
      },
      "user": {
        "id": "WGZ9C4kBq594J-K-pLQ7cz6J1L2R9sBJvDOhV-5YkAQ",
        "name": "username@foo.com",
        "displayName": "Foo User"
      },
      "pubKeyCredParams": [
        {
          "type": "public-key",
          "alg": -7
        }
      ]
    }
    """
GET_PUBLIC_KEY_OPTIONS = """
    {
      "challenge": "jdrE6w1jAk2D1eJttNdOb9jTP5788BWlPyhA4ZqCDNdrRS1yaqU-DBD_H6rKaGe1t-2xoYNLYbUVKiOEVmaxNg"
    }
    """


def public_key_options(method):
    if method == "CreateCredential":
        return CREATE_PUBLIC_KEY_OPTIONS
    return GET_PUBLIC_KEY_OPTIONS


@pytest.fixture
def required_templates():
    return {"credential": {}}


@pytest.fixture
def xdp_overwrite_env():
    return {"XDG_DESKTOP_PORTAL_ENABLE_EXPERIMENTAL": "Credential"}


@pytest.fixture
def xdg_desktop_portal_dir_files():
    return {
        "credential.portal": b"""
[portal]
DBusName=xyz.iinuwa.credentialsd.Credentials
Interfaces=org.freedesktop.impl.portal.experimental.Credential
""",
        "test-portals.conf": b"""
[preferred]
default=test;
org.freedesktop.impl.portal.experimental.Credential=credential;
""",
    }


def call_credential(dbus_con, method, options, origin=ORIGIN):
    credential_intf = xdp.get_iface(
        dbus_con, "org.freedesktop.portal.experimental.Credential"
    )
    request = xdp.Request(dbus_con, credential_intf)
    args = {"parent_window": "", "origin": origin}
    if method == "CreateCredential":
        args["type"] = "publicKey"
    return request.call(method, **args, options=options)


def last_handler_options(dbus_con, method):
    mock_intf = xdp.get_mock_iface(dbus_con, HANDLER_BUS_NAME)
    method_calls = mock_intf.GetMethodCalls(method)
    assert len(method_calls) == 1
    _, args = method_calls[-1]
    return args[-2]


def test_version(portals, dbus_con):
    """tests the version of the interface"""

    xdp.check_version(dbus_con, "experimental.Credential", 1)


@pytest.mark.parametrize("method", ["CreateCredential", "GetCredential"])
class TestCredential:
    def test_same_origin(self, portals, dbus_con, method):
        """public_key is a string, and top_origin is omitted for same-origin"""

        response = call_credential(
            dbus_con, method, {"public_key": public_key_options(method)}
        )

        assert response
        assert response.response == 0
        options = last_handler_options(dbus_con, method)
        assert options["public_key"] == public_key_options(method)
        assert "top_origin" not in options

    def test_cross_origin(self, portals, dbus_con, method):
        """a non-empty top_origin is forwarded to the handler"""

        top_origin = "https://top.example.org"
        response = call_credential(
            dbus_con,
            method,
            {
                "public_key": public_key_options(method),
                "top_origin": top_origin,
            },
        )

        assert response
        assert response.response == 0
        options = last_handler_options(dbus_con, method)
        assert options["top_origin"] == top_origin

    def test_empty_top_origin(self, portals, dbus_con, method):
        """an empty top_origin is rejected instead of reaching the handler"""

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            call_credential(
                dbus_con,
                method,
                {"public_key": public_key_options(method), "top_origin": ""},
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        mock_intf = xdp.get_mock_iface(dbus_con, HANDLER_BUS_NAME)
        assert not mock_intf.GetMethodCalls(method)

    def test_public_key_dict(self, portals, dbus_con, method):
        """public_key as a dictionary is rejected instead of reaching the handler"""

        public_key = {"request_json": public_key_options(method)}
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            call_credential(dbus_con, method, {"public_key": public_key})
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        mock_intf = xdp.get_mock_iface(dbus_con, HANDLER_BUS_NAME)
        assert not mock_intf.GetMethodCalls(method)

    def test_empty_origin(self, portals, dbus_con, method):
        """an empty origin is rejected instead of reaching the handler"""

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            call_credential(
                dbus_con,
                method,
                {"public_key": public_key_options(method)},
                origin="",
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        mock_intf = xdp.get_mock_iface(dbus_con, HANDLER_BUS_NAME)
        assert not mock_intf.GetMethodCalls(method)
