import os
import time
import tests as xdp
import random
import dbus
from gi.repository import GLib, Gio
import pytest
import json

def set_web_extensions_permissions(permission, name, app_id, dbus_con):
    permission_store = xdp.get_permission_store_iface(dbus_con)
    try:
        permission_store.SetPermission(
            "webextensions", True, "org.example.testing", app_id, [permission]
        )
    except dbus.DBusException as e:
        raise RuntimeError(f"Failed to set permissions: {e}")

class TestWebExtensions:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "WebExtensions", 1)

    def create_session(self, dbus_con):
        webextensions_intf = xdp.get_portal_iface(dbus_con, "WebExtensions")
        session_token = "portal" + str(random.randint(0, 100000))
        options = {
            "mode": "mozilla",
            "session_handle_token": session_token
        }
        session = xdp.Session(
            dbus_con,
            webextensions_intf.CreateSession(options),
        )
        return session.handle

    def get_manifest(self, name, extension_or_origin, dbus_con):
        webextensions_intf = xdp.get_portal_iface(dbus_con, "WebExtensions")
        return webextensions_intf.GetManifest(
            self.session_handle,
            name,
            extension_or_origin
        )

    def start(self, name, extension_or_origin, dbus_con):
        webextensions_intf = xdp.get_portal_iface(dbus_con, "WebExtensions")
        request = xdp.Request(dbus_con, webextensions_intf)
        return request.call(
                "Start", 
                session_handle=self.session_handle, 
                name=name, 
                extension_or_origin=extension_or_origin,
                options={}
        )

    def get_pipes(self, dbus_con):
        webextensions_intf = xdp.get_portal_iface(dbus_con, "WebExtensions")
        response = webextensions_intf.GetPipes(
            self.session_handle,
            {}
        )
        return response
    
    def test_web_extensions_basic(self, portals, app_id, dbus_con):
        name = "org.example.testing"
        set_web_extensions_permissions("yes", name, app_id, dbus_con)

        extension_or_origin = "some-extension@example.org"

        self.session_handle = self.create_session(dbus_con)
        assert self.session_handle, "Session handle should not be None"

        manifest_data = self.get_manifest(name, extension_or_origin, dbus_con)
        assert manifest_data, "Manifest should not be None"

        manifest =json.loads(manifest_data)
        assert "some-extension@example.org" in manifest['allowed_extensions'], \
                "Extension ID missing in manifest"

        response = self.start(name, extension_or_origin, dbus_con)
        assert response, "Response cannot be None"
        assert response.response == 0, "Returned invalid response code, expected 0"

        pipes = self.get_pipes(dbus_con)
        assert pipes, "No pipes returned"
        assert pipes[0].take() > 0, "stdin file descriptor should be valid"
        assert pipes[1].take() > 0, "stdout file descriptor should be valid"
        assert pipes[2].take() > 0, "stderr file descriptor should be valid"

    def test_web_extensions_denied(self, portals, app_id, dbus_con):
        name = "org.example.testing"
        set_web_extensions_permissions("no", name, app_id, dbus_con)

        extension_or_origin = "some-extension@example.org"

        self.session_handle = self.create_session(dbus_con)
        assert self.session_handle, "Session handle should not be None"

        manifest_data = self.get_manifest(name, extension_or_origin, dbus_con)
        assert manifest_data, "Manifest should not be None"

        manifest =json.loads(manifest_data)
        assert "some-extension@example.org" in manifest['allowed_extensions'], \
                "Extension ID missing in manifest"

        response = self.start(name, extension_or_origin, dbus_con)
        print(response)
        assert response, "Response cannot be None"
        assert response.response == 1, "Returned invalid response code, expected 1"

        pipes = None
        try:
            pipes = self.get_pipes(dbus_con)
        except dbus.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.DBus.Error.AccessDenied", "unknown exception"
        assert pipes == None, "Expected pipes == None"

    def test_web_extensions_bad_name(self, portals, app_id, dbus_con):
        bad_extension_names = ["no-dashes", "../foo", "no_trailing_dot."]

        name = "org.example.testing"
        set_web_extensions_permissions("yes", name, app_id, dbus_con)
        
        self.session_handle = self.create_session(dbus_con)
        assert self.session_handle, "Session handle should not be None"

        extension_or_origin = "some-extension@example.org"

        for extension_or_origin in bad_extension_names:
          self.session_handle = self.create_session(dbus_con)
          response = self.start(name, extension_or_origin, dbus_con)
          assert response, "Response cannot be None"
          assert response.response == 2, "Returned invalid response, expected 2"
