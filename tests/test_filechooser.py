# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest


FILECHOOSER_RESULTS = {
    "uris": ["file:///test.txt", "file:///example/test2.txt"],
    "choices": [("encoding", "utf8"), ("reencode", "true"), ("third", "a")],
}


@pytest.fixture
def required_templates():
    return {
        "filechooser": {
            "results": dbus.Dictionary(FILECHOOSER_RESULTS, signature="sv"),
        },
        "lockdown": {},
    }


class TestFilechooser:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "FileChooser", 4)

    def test_open_file_basic(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test"
        accept_label = "Accept"
        multiple = True
        options = {
            "accept_label": accept_label,
            "multiple": multiple,
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "OpenFile",
            parent_window="",
            title=title,
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4]["accept_label"] == accept_label
        assert args[4]["multiple"] == multiple

    @pytest.mark.parametrize("template_params", ({"filechooser": {"response": 1}},))
    def test_open_file_cancel(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test"
        accept_label = "Accept"
        multiple = True
        options = {
            "accept_label": accept_label,
            "multiple": multiple,
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "OpenFile",
            parent_window="",
            title=title,
            options=options,
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4]["accept_label"] == accept_label
        assert args[4]["multiple"] == multiple

    @pytest.mark.parametrize(
        "template_params", ({"filechooser": {"expect-close": True}},)
    )
    def test_open_file_close(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test"
        accept_label = "Accept"
        multiple = True
        options = {
            "accept_label": accept_label,
            "multiple": multiple,
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        request.schedule_close(1000)
        request.call(
            "OpenFile",
            parent_window="",
            title=title,
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4]["accept_label"] == accept_label
        assert args[4]["multiple"] == multiple

    def test_open_file_filter1(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        options = {
            "filters": [
                (
                    "Images",
                    [
                        (dbus.UInt32(0), "*ico"),
                        (dbus.UInt32(1), "image/png"),
                    ],
                ),
                (
                    "Text",
                    [
                        (dbus.UInt32(0), "*.txt"),
                    ],
                ),
            ],
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "OpenFile",
            parent_window="",
            title="Test",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1

    def test_open_file_filter2(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")

        options = {
            "filters": [
                (
                    "Text",
                    [
                        # Invalid filter type
                        (dbus.UInt32(4), "*.txt"),
                    ],
                ),
            ],
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        try:
            request.call(
                "OpenFile",
                parent_window="",
                title="Test",
                options=options,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"

    def test_open_file_current_filter1(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        options = {
            "filters": [
                (
                    "Images",
                    [
                        (dbus.UInt32(0), "*ico"),
                        (dbus.UInt32(1), "image/png"),
                    ],
                ),
                (
                    "Text",
                    [
                        (dbus.UInt32(0), "*.txt"),
                    ],
                ),
            ],
            "current_filter": (
                "Text",
                [
                    (dbus.UInt32(0), "*.txt"),
                ],
            ),
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "OpenFile",
            parent_window="",
            title="Test",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[4]["current_filter"] == options["current_filter"]

    def test_open_file_current_filter2(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        options = {
            "current_filter": (
                "Text",
                [
                    (dbus.UInt32(0), "*.txt"),
                ],
            ),
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "OpenFile",
            parent_window="",
            title="Test",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[4]["current_filter"] == options["current_filter"]

    def test_open_file_current_filter3(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")

        options = {
            "current_filter": (
                "Text",
                [
                    # Invalid filter type
                    (dbus.UInt32(6), "*.txt"),
                ],
            ),
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        try:
            request.call(
                "OpenFile",
                parent_window="",
                title="Test",
                options=options,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"

    def test_open_file_current_filter4(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")

        options = {
            "filters": [
                (
                    "Images",
                    [
                        (dbus.UInt32(0), "*ico"),
                        (dbus.UInt32(1), "image/png"),
                    ],
                ),
                (
                    "Text",
                    [
                        (dbus.UInt32(0), "*.txt"),
                    ],
                ),
            ],
            "current_filter": (
                "Something else",
                [
                    (dbus.UInt32(0), "*.sth.else"),
                ],
            ),
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        try:
            request.call(
                "OpenFile",
                parent_window="",
                title="Test",
                options=options,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"

    def test_open_file_choices1(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        options = {
            "choices": [
                (
                    "encoding",
                    "Encoding",
                    [
                        ("utf8", "Unicode"),
                        ("latin15", "Western"),
                    ],
                    "latin15",
                ),
                (
                    "reencode",
                    "Reencode",
                    [],
                    "false",
                ),
                (
                    "third",
                    "Third",
                    [("a", "A"), ("b", "B")],
                    "",
                ),
            ],
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "OpenFile",
            parent_window="",
            title="Test",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        method_calls = mock_intf.GetMethodCalls("OpenFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[4]["choices"] == options["choices"]

    def test_open_file_choices_invalid(self, portals, dbus_con):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")

        invalid_choices = [
            (
                "encoding",
                "Encoding",
                [
                    ("utf8", ""),
                    ("latin15", "Western"),
                ],
                "latin15",
            ),
            (
                "encoding",
                "Encoding",
                [
                    ("", "Unicode"),
                    ("latin15", "Western"),
                ],
                "latin15",
            ),
            (
                "",
                "Encoding",
                [
                    ("utf8", "Unicode"),
                    ("latin15", "Western"),
                ],
                "latin15",
            ),
        ]

        for choice in invalid_choices:
            request = xdp.Request(dbus_con, filechooser_intf)
            try:
                options = {
                    "choices": [choice],
                }
                request.call(
                    "OpenFile",
                    parent_window="",
                    title="Test",
                    options=options,
                )
                assert False, "This statement should not be reached"
            except dbus.exceptions.DBusException as e:
                assert (
                    e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"
                )

    def test_save_file_basic(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test"
        accept_label = "Accept"
        current_name = "File Name"
        options = {
            "accept_label": accept_label,
            "current_name": current_name,
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "SaveFile",
            parent_window="",
            title=title,
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SaveFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4]["accept_label"] == accept_label
        assert args[4]["current_name"] == current_name

    @pytest.mark.parametrize("template_params", ({"filechooser": {"response": 1}},))
    def test_save_file_cancel(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test"
        accept_label = "Accept"
        current_name = "File Name"
        options = {
            "accept_label": accept_label,
            "current_name": current_name,
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "SaveFile",
            parent_window="",
            title=title,
            options=options,
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SaveFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4]["accept_label"] == accept_label
        assert args[4]["current_name"] == current_name

    @pytest.mark.parametrize(
        "template_params", ({"filechooser": {"expect-close": True}},)
    )
    def test_save_file_close(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        title = "Test"
        accept_label = "Accept"
        current_name = "File Name"
        options = {
            "accept_label": accept_label,
            "current_name": current_name,
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        request.schedule_close(1000)
        request.call(
            "SaveFile",
            parent_window="",
            title=title,
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SaveFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[3] == title
        assert args[4]["accept_label"] == accept_label
        assert args[4]["current_name"] == current_name

    def test_save_file_filters(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        options = {
            "filters": [
                (
                    "Images",
                    [
                        (dbus.UInt32(0), "*ico"),
                        (dbus.UInt32(1), "image/png"),
                    ],
                ),
                (
                    "Text",
                    [
                        (dbus.UInt32(0), "*.txt"),
                    ],
                ),
            ],
        }
        request = xdp.Request(dbus_con, filechooser_intf)
        response = request.call(
            "SaveFile",
            parent_window="",
            title="Title",
            options=options,
        )

        assert response
        assert response.response == 0
        assert response.results["uris"] == FILECHOOSER_RESULTS["uris"]

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("SaveFile")
        assert len(method_calls) == 1
        _, args = method_calls.pop()
        assert args[4]["filters"] == options["filters"]

    @pytest.mark.parametrize(
        "template_params", ({"lockdown": {"disable-save-to-disk": True}},)
    )
    def test_save_file_lockdown(self, portals, dbus_con, app_id):
        filechooser_intf = xdp.get_portal_iface(dbus_con, "FileChooser")
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, filechooser_intf)
        try:
            request.call(
                "SaveFile",
                parent_window="",
                title="Title",
                options={},
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"

        # Check the impl portal was not called
        method_calls = mock_intf.GetMethodCalls("FileChooser")
        assert len(method_calls) == 0
