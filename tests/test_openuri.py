# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp
import tests.xdp_doc_utils as xdp_doc

import dbus
import pytest
import os
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
        "applications/furrfix2.desktop": furrfix_desktop,
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

    def test_http1(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
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

    def test_file(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "text/plain"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        file_path = Path.home() / "openuri_mock_file"
        file_path.write_text("openuri_mock_file")
        fd = os.open(file_path.absolute().as_posix(), os.O_RDONLY)

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

        os.close(fd)

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
    def test_close(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
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
    def test_lockdown(self, portals, dbus_con):
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

    def test_dir(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        mock_intf = xdp.get_mock_iface(dbus_con)

        scheme_handler = "inode/directory"
        self.enable_paranoid_mode(dbus_con, scheme_handler)

        file_path = Path.home() / "openuri_mock_file"
        file_path.write_text("openuri_mock_file")
        fd = os.open(file_path.absolute().as_posix(), os.O_RDONLY)

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

        os.close(fd)

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

    # tests mapping from
    # /run/user/1000/doc/_id_/openuri-test -> /home/user/openuri-test
    @pytest.mark.parametrize(
        "path", ("file.html", "dir/file.html", "dir/subdir/file.html")
    )
    def test_openfile_opens_host_path(
        self, xdg_document_portal, portals, dbus_con, xdp_app_info, path
    ):
        app_id = xdp_app_info.app_id
        openuri_intf = xdp.get_portal_iface(dbus_con, "OpenURI")
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = xdp_doc.get_mountpoint(documents_intf)

        # create directory in host which will be added to document portal
        export_path = Path.home() / "openuri-test"
        export_path.mkdir(parents=True)
        doc_ids = xdp_doc.export_files(
            documents_intf,
            [export_path],
            ["read", "write"],
            flags=xdp_doc.EXPORT_FILES_FLAG_EXPORT_DIR,
        )
        assert doc_ids
        doc_id = doc_ids[0][0]
        assert doc_id

        # create file in the directory which was added to document directory
        file_host_path = export_path / path
        file_doc_path = mountpoint / doc_id / "openuri-test" / path

        file_host_path.parent.mkdir(parents=True, exist_ok=True)
        file_host_path.write_bytes(b"openuri_mock_file_content")

        # Call OpenFile by using fd
        with open(file_doc_path) as f:
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
        assert path == "file://" + file_host_path.absolute().as_posix()
        assert file_doc_path != file_host_path.absolute().as_posix()
