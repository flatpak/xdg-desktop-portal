# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest
import os
import tempfile
from pathlib import Path
from typing import Any


defaults_list = b"""[Default Applications]
x-scheme-handler/http=furrfix.desktop;
text/plain=furrfix.desktop
"""

furrfix_desktop = b"""[Desktop Entry]
Version=1.0
Name=Furrfix
GenericName=Not a Web Browser
Comment=Don't Browse the Web
Exec=true %u
Icon=furrfix
Terminal=false
Type=Application
MimeType=text/plain;text/html;text/xml;application/xhtml+xml;application/vnd.mozilla.xul+xml;text/mml;x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/xdg-desktop-portal-test;
StartupNotify=true
Categories=Network;WebBrowser;
Keywords=web;browser;internet;
"""
furrfix_desktop2 = b"""[Desktop Entry]
Version=1.0
Name=Furrfix2
GenericName=Not a Web Browser 2
Comment=Don't Browse the Web
Exec=true %u
Icon=furrfix2
Terminal=false
Type=Application
MimeType=text/plain;text/html;text/xml;application/xhtml+xml;application/vnd.mozilla.xul+xml;text/mml;x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/xdg-desktop-portal-test;
StartupNotify=true
Categories=Network;WebBrowser;
Keywords=web;browser;internet;
"""


mimeinfo_cache = b"""[MIME Cache]
application/vnd.mozilla.xul+xml=furrfix.desktop;furrfix2.desktop;
application/xhtml+xml=furrfix.desktop;furrfix2.desktop;
text/plain=furrfix.desktop;furrfix2.desktop;
text/html=furrfix.desktop;furrfix2.desktop;
text/mml=furrfix.desktop;furrfix2.desktop;
text/xml=furrfix.desktop;furrfix2.desktop;
x-scheme-handler/http=furrfix.desktop;furrfix2.desktop;
x-scheme-handler/https=furrfix.desktop;furrfix2.desktop;
x-scheme-handler/xdg-desktop-portal-test=furrfix.desktop;furrfix2.desktop;
"""


@pytest.fixture
def xdg_data_home_files():
    return {
        "applications/defaults.list": defaults_list,
        "applications/furrfix.desktop": furrfix_desktop,
        "applications/furrfix2.desktop": furrfix_desktop2,
        "applications/mimeinfo.cache": mimeinfo_cache,
    }


@pytest.fixture
def required_templates():
    return {
        "appchooser": {},
        "lockdown": {},
    }


class TestOpenURI:
    def set_permissions(self, dbus_con, type, permissions):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetPermission(
            "desktop-used-apps",
            True,
            "inhibit",
            type,
            permissions,
        )

    def enable_paranoid_mode(self, dbus_con, type):
        # turn on paranoid mode to ensure we get a backend call
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetValue(
            "desktop-used-apps",
            True,
            type,
            dbus.Dictionary(
                {
                    "always-ask": True,
                },
                signature="sv",
            ),
        )

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "OpenURI", 5)

    def test_http1(self, portals, dbus_con, app_id):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "x-scheme-handler/http"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        uri = "http://www.flatpak.org"
        writable = False
        activation_token = "token"

        request = xdp.Request(dbus_con, openuri_intf)
        options = {
            "writable": writable,
            "activation_token": activation_token,
        }
        response = request.call(
            "OpenURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert "furrfix" in args[3]
        assert args[4]["uri"] == uri
        assert args[4]["content_type"] == scheme_handler
        assert args[4]["activation_token"] == activation_token

    def test_http2(self, portals, dbus_con):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "x-scheme-handler/http"
        self.set_permissions(dbus_con, scheme_handler, ["furrfix", "3", "3"])

        uri = "http://www.flatpak.org"
        writable = False
        activation_token = "token"

        request = xdp.Request(dbus_con, openuri_intf)
        options = {
            "writable": writable,
            "activation_token": activation_token,
        }
        response = request.call(
            "OpenURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was not called because the choice thresold
        # has been reached
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) == 0

    def test_file(self, portals, dbus_con, app_id):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "text/plain"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        fd, _ = tempfile.mkstemp(prefix="openuri_mock_file_", dir=Path.home())
        os.write(fd, b"openuri_mock_file")

        writable = False
        activation_token = "token"

        request = xdp.Request(dbus_con, openuri_intf)
        options = {
            "writable": writable,
            "activation_token": activation_token,
        }
        response = request.call(
            "OpenFile",
            parent_window="",
            fd=fd,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert "furrfix" in args[3]
        assert args[4]["content_type"] == scheme_handler
        assert args[4]["activation_token"] == activation_token

        path = args[4]["uri"]
        assert path.startswith("file:///")

        with open(path[7:]) as file:
            openuri_file_contents = file.read()
            assert openuri_file_contents == "openuri_mock_file"

    @pytest.mark.parametrize("template_params", ({"appchooser": {"response": 1}},))
    def test_cancel(self, portals, dbus_con):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")

        scheme_handler = "x-scheme-handler/http"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        uri = "http://www.flatpak.org"

        request = xdp.Request(dbus_con, openuri_intf)
        options: Any = {}
        response = request.call(
            "OpenURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        assert response
        assert response.response == 1

    @pytest.mark.parametrize(
        "template_params", ({"appchooser": {"expect-close": True}},)
    )
    def test_close(self, portals, dbus_con, app_id):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "x-scheme-handler/http"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        uri = "http://www.flatpak.org"
        writable = False
        activation_token = "token"

        request = xdp.Request(dbus_con, openuri_intf)
        request.schedule_close(1000)
        options = {
            "writable": writable,
            "activation_token": activation_token,
        }
        request.call(
            "OpenURI",
            parent_window="",
            uri=uri,
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert "furrfix" in args[3]
        assert args[4]["uri"] == uri
        assert args[4]["content_type"] == scheme_handler
        assert args[4]["activation_token"] == activation_token

    @pytest.mark.parametrize(
        "template_params", ({"lockdown": {"disable-application-handlers": True}},)
    )
    def test_lockdown(self, portals, dbus_con, app_id):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")

        scheme_handler = "x-scheme-handler/http"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        uri = "http://www.flatpak.org"
        writable = False
        activation_token = "token"

        request = xdp.Request(dbus_con, openuri_intf)
        options = {
            "writable": writable,
            "activation_token": activation_token,
        }
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "OpenURI",
                parent_window="",
                uri=uri,
                options=options,
            )
        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )

    def test_dir(self, portals, dbus_con, app_id):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "inode/directory"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        fd, file_path = tempfile.mkstemp(prefix="openuri_mock_file_", dir=Path.home())
        os.write(fd, b"openuri_mock_file")

        activation_token = "token"

        request = xdp.Request(dbus_con, openuri_intf)
        options = {
            "activation_token": activation_token,
        }
        response = request.call(
            "OpenDirectory",
            parent_window="",
            fd=fd,
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the appchooser portal got called to open the containing dir
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert args[4]["content_type"] == scheme_handler
        assert args[4]["activation_token"] == activation_token

        path = args[4]["uri"]
        assert path.startswith("file:///")

        assert Path(path[7:]) == Path(file_path).parent

    def test_scheme_supported(self, portals, dbus_con):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")

        supported = openuri_intf.SchemeSupported("https", {})
        assert supported

        supported = openuri_intf.SchemeSupported("bogusnonexistanthandler", {})
        assert not supported

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            openuri_intf.SchemeSupported("", {})
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

    # tests mapping from /run/user/1000/doc/_id_/file.html -> /home/user/file.html
    def test_openfile_opens_host_path(
        self, portals, xdg_document_portal, dbus_con, app_id
    ):
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        documents_intf = xdp.get_document_portal_iface(dbus_con)

        stored_fd, file_name = tempfile.mkstemp(
            prefix="openuri_mock_file_", suffix=".html", dir=Path.home()
        )
        os.write(stored_fd, b"openuri_mock_file_content")
        os.close(stored_fd)

        file_path = Path(file_name)
        doc_id = xdp.export_file(documents_intf, file_path)
        mountpoint = xdp.get_mountpoint(documents_intf)
        doc_path = mountpoint / doc_id / file_name
        documents_intf.GrantPermissions(doc_id, "org.example.Test", ["read"])

        # Call OpenFile by using fd
        with open(doc_path) as f:
            fd = f.fileno()
            assert fd
            activation_token = "token"
            request = xdp.Request(dbus_con, openuri_intf)
            options = {
                "writable": False,
                "activation_token": activation_token,
            }

            response = request.call(
                "OpenFile",
                parent_window="",
                fd=fd,
                options=options,
            )
            assert response
            assert response.response == 0

        # Check the impl portal was called with the right args
        mock_intf = xdp.get_mock_iface(dbus_con)
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert "furrfix" in args[3]

        assert args[4]["activation_token"] == activation_token

        path = args[4]["uri"]
        assert path == "file://" + file_name
        assert doc_path != file_name

    # tests mapping from /run/user/1000/doc/_id_/dir/file.html -> /home/user/dir/file.html
    def test_openfile_opens_host_path_in_dir(
        self, portals, xdg_document_portal, dbus_con, app_id
    ):
        documents_intf = xdp.get_document_portal_iface(dbus_con)

        # create directory in host which will be added to document portal
        host_dir_name = tempfile.mkdtemp(prefix="openuri_mock_dir_", dir=Path.home())
        host_dir_path = Path(host_dir_name)
        doc_ids = xdp.export_files(
            documents_intf,
            [host_dir_path],
            ["read", "write"],
            flags=xdp.EXPORT_FILES_FLAG_EXPORT_DIR,
        )
        assert doc_ids
        doc_id = doc_ids[0][0]
        assert doc_id

        # create file in the directory which was added to document directory
        mountpoint = xdp.get_mountpoint(documents_intf)
        stored_filename = "file.html"
        stored_host_filepath = (host_dir_path / stored_filename).as_posix()
        with open(
            mountpoint / doc_id / os.path.basename(host_dir_name) / stored_filename, "w"
        ) as f:
            f.write("openuri_mock_file_content")

        # open fd in the document path
        doc_path = (
            mountpoint / doc_id / os.path.basename(host_dir_name) / stored_filename
        )
        with open(doc_path) as f:
            fd = f.fileno()
            assert fd

            activation_token = "token"
            openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
            request = xdp.Request(dbus_con, openuri_intf)
            options = {
                "writable": False,
                "activation_token": activation_token,
            }

            response = request.call(
                "OpenFile",
                parent_window="",
                fd=fd,
                options=options,
            )
            assert response
            assert response.response == 0

        # Check the impl portal was called with the right args
        mock_intf = xdp.get_mock_iface(dbus_con)
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert "furrfix" in args[3]
        assert args[4]["activation_token"] == activation_token
        path = args[4]["uri"]
        assert path == "file://" + stored_host_filepath

    # Tests mapping from /run/user/1000/doc/_id_/dir/subdir/file.html -> /home/user/dir/subdir/file.html
    def test_openfile_opens_host_path_in_subdir(
        self, portals, xdg_document_portal, dbus_con, app_id
    ):
        documents_intf = xdp.get_document_portal_iface(dbus_con)

        # create directory in host which will be added to document portal
        host_dir_name = tempfile.mkdtemp(prefix="openuri_mock_dir_", dir=Path.home())
        host_dir_path = Path(host_dir_name)
        doc_ids = xdp.export_files(
            documents_intf,
            [host_dir_path],
            ["read", "write"],
            flags=xdp.EXPORT_FILES_FLAG_EXPORT_DIR,
        )
        assert doc_ids
        doc_id = doc_ids[0][0]
        assert doc_id

        # create dir and file into that dir in the dir which was added to document directory
        mountpoint = xdp.get_mountpoint(documents_intf)
        os.makedirs(host_dir_path / "new_dir")
        stored_filename = "file.html"
        with open(
            mountpoint
            / doc_id
            / os.path.basename(host_dir_name)
            / "new_dir"
            / stored_filename,
            "w",
        ) as f:
            f.write("openuri_mock_file_content")
        stored_host_filepath = (host_dir_path / "new_dir" / stored_filename).as_posix()

        doc_path = (
            mountpoint
            / doc_id
            / os.path.basename(host_dir_name)
            / "new_dir"
            / stored_filename
        )

        # open fd in the document path
        with open(doc_path) as f:
            fd = f.fileno()
            assert fd

            activation_token = "token"
            openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
            request = xdp.Request(dbus_con, openuri_intf)
            options = {
                "writable": False,
                "activation_token": activation_token,
            }

            response = request.call(
                "OpenFile",
                parent_window="",
                fd=fd,
                options=options,
            )
            assert response
            assert response.response == 0

        # Check the impl portal was called with the right args
        mock_intf = xdp.get_mock_iface(dbus_con)
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[1] == app_id
        assert args[2] == ""  # parent window
        assert "furrfix" in args[3]
        assert args[4]["activation_token"] == activation_token
        path = args[4]["uri"]
        assert path == "file://" + stored_host_filepath
