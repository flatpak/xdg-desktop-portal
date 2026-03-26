# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
import os
import tempfile
from pathlib import Path
from typing import Any


PRINT_PREPARE_DATA = {
    "token": dbus.UInt32(1337),
}


@pytest.fixture
def required_templates():
    return {
        "print": {
            "prepare-results": PRINT_PREPARE_DATA,
        },
        "lockdown": {},
    }


class TestPrint:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Print", 5)

    def test_prepare_print_basic(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test Title"
        settings: Any = {}
        page_setup: Any = {}
        options = {
            "modal": True,
            "accept_label": "Accept",
            "supported_output_file_formats": ["pdf"],
        }

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "PreparePrint",
            parent_window="",
            title=title,
            settings=settings,
            page_setup=page_setup,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PreparePrint")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4] == settings
        assert args[5] == page_setup
        assert args[6]["modal"] == options["modal"]
        assert args[6]["accept_label"] == options["accept_label"]
        assert (
            args[6]["supported_output_file_formats"]
            == options["supported_output_file_formats"]
        )

    @pytest.mark.parametrize("template_params", ({"print": {"response": 1}},))
    def test_prepare_print_cancel(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test Title"

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "PreparePrint",
            parent_window="",
            title=title,
            settings={},
            page_setup={},
            options={},
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PreparePrint")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title

    @pytest.mark.parametrize("template_params", ({"print": {"expect-close": True}},))
    def test_prepare_print_close(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test Title"

        request = xdp.Request(dbus_con, print_intf)
        request.schedule_close(1000)
        request.call(
            "PreparePrint",
            parent_window="",
            title=title,
            settings={},
            page_setup={},
            options={},
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PreparePrint")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title

    @pytest.mark.parametrize(
        "template_params", ({"lockdown": {"disable-printing": True}},)
    )
    def test_prepare_print_lockdown(self, portals, dbus_con):
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test Title"

        request = xdp.Request(dbus_con, print_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "PreparePrint",
                parent_window="",
                title=title,
                settings={},
                page_setup={},
                options={},
            )
        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )

        # Check the impl portal was not called
        method_calls = mock_intf.GetMethodCalls("PreparePrint")
        assert len(method_calls) == 0

    def test_print_basic(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd, file_path = tempfile.mkstemp(prefix="print_mock_file_", dir=Path.home())
        os.write(fd, b"print_mock_file")

        title = "Test Title"
        options = {
            "modal": True,
            "token": "token",
            "supported_output_file_formats": ["svg"],
        }

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "Print",
            parent_window="",
            title=title,
            fd=fd,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Print")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[5]["modal"] == options["modal"]
        assert (
            args[5]["supported_output_file_formats"]
            == options["supported_output_file_formats"]
        )

        backend_fd = args[4].take()
        ino = os.stat(f"/proc/self/fd/{fd}").st_ino
        ino_backend = os.stat(f"/proc/self/fd/{backend_fd}").st_ino
        os.close(fd)
        assert ino == ino_backend

    @pytest.mark.parametrize("template_params", ({"print": {"response": 1}},))
    def test_print_cancel(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd, _ = tempfile.mkstemp(prefix="print_mock_file_", dir=Path.home())
        os.write(fd, b"print_mock_file")

        title = "Test Title"

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "Print",
            parent_window="",
            title=title,
            fd=fd,
            options={},
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Print")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id

    @pytest.mark.parametrize("template_params", ({"print": {"expect-close": True}},))
    def test_print_close(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd, _ = tempfile.mkstemp(prefix="print_mock_file_", dir=Path.home())
        os.write(fd, b"print_mock_file")

        title = "Test Title"

        request = xdp.Request(dbus_con, print_intf)
        request.schedule_close(1000)
        request.call(
            "Print",
            parent_window="",
            title=title,
            fd=fd,
            options={},
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Print")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id

    @pytest.mark.parametrize(
        "template_params", ({"lockdown": {"disable-printing": True}},)
    )
    def test_print_lockdown(self, portals, dbus_con):
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd, _ = tempfile.mkstemp(prefix="print_mock_file_", dir=Path.home())
        os.write(fd, b"print_mock_file")

        title = "Test Title"

        request = xdp.Request(dbus_con, print_intf)
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "Print",
                parent_window="",
                title=title,
                fd=fd,
                options={},
            )
        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )

        # Check the impl portal was not called
        method_calls = mock_intf.GetMethodCalls("Print")
        assert len(method_calls) == 0

    def test_print_prepare_and_print(self, portals, dbus_con):
        print_intf = xdp.get_portal_iface(dbus_con, "Print")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test Title"

        fd, file_path = tempfile.mkstemp(prefix="print_mock_file_", dir=Path.home())
        os.write(fd, b"print_mock_file")

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "PreparePrint",
            parent_window="",
            title=title,
            settings={},
            page_setup={},
            options={},
        )

        assert response
        assert response.response == 0
        assert response.results["token"] == PRINT_PREPARE_DATA["token"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("PreparePrint")
        assert len(method_calls) == 1

        options = {
            "token": response.results["token"],
        }

        request = xdp.Request(dbus_con, print_intf)
        response = request.call(
            "Print",
            parent_window="",
            title=title,
            fd=fd,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("Print")
        assert len(method_calls) == 1
        _, args = method_calls.pop()

        assert args[5]["token"] == PRINT_PREPARE_DATA["token"]

        backend_fd = args[4].take()
        ino = os.stat(f"/proc/self/fd/{fd}").st_ino
        ino_backend = os.stat(f"/proc/self/fd/{backend_fd}").st_ino
        os.close(fd)
        assert ino == ino_backend
