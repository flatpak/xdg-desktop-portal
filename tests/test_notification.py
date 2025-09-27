# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest
import tempfile
import os
from pathlib import Path
from gi.repository import GLib, Gio

SVG_IMAGE_DATA = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" height="16px" width="16px"/>
"""

SOUND_DATA = (
    b"\x52\x49\x46\x46\x24\x00\x00\x00\x57\x41\x56\x45" +
    b"\x66\x6d\x74\x20\x10\x00\x00\x00\x01\x00\x01\x00" +
    b"\x44\xac\x00\x00\x88\x58\x01\x00\x02\x00\x10\x00" +
    b"\x64\x61\x74\x61\x00\x00\x00\x00"
)  # fmt: skip


SUPPORTED_OPTIONS = {
    "foo": "bar",
}

NOTIFICATION_BASIC = {
    "title": GLib.Variant("s", "title"),
    "body": GLib.Variant("s", "test notification body"),
    "priority": GLib.Variant("s", "normal"),
    "default-action": GLib.Variant("s", "test-action"),
}

NOTIFICATION_BUTTONS = {
    "title": GLib.Variant("s", "test notification 2"),
    "body": GLib.Variant("s", "test notification body 2"),
    "priority": GLib.Variant("s", "low"),
    "default-action": GLib.Variant("s", "test-action"),
    "buttons": GLib.Variant(
        "aa{sv}",
        [
            {
                "label": GLib.Variant("s", "button1"),
                "action": GLib.Variant("s", "action1"),
            },
            {
                "label": GLib.Variant("s", "button2"),
                "action": GLib.Variant("s", "action2"),
            },
        ],
    ),
}


@pytest.fixture
def required_templates():
    return {
        "notification": {
            "SupportedOptions": SUPPORTED_OPTIONS,
        },
    }


class NotificationPortal(xdp.GDBusIface):
    def __init__(self):
        super().__init__(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Notification",
        )

    def AddNotification(self, id, notification, fds=[]):
        return self._call(
            "AddNotification",
            GLib.Variant("(sa{sv})", (id, notification)),
            fds,
        )

    def RemoveNotification(self, id):
        return self._call(
            "RemoveNotification",
            GLib.Variant("(s)", (id,)),
        )


class TestNotification:
    def add_notification(self, dbus_con, app_id, id, notification, fds=[]):
        notification_intf = NotificationPortal()
        mock_intf = xdp.get_mock_iface(dbus_con)

        method_calls = mock_intf.GetMethodCalls("AddNotification")
        backend_calls = len(method_calls)

        notification_intf.AddNotification(id, notification, fds)

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AddNotification")
        assert len(method_calls) == backend_calls + 1
        _, args = method_calls[-1]
        assert args[0] == app_id
        assert args[1] == id

        return args[2]

    def check_notification(
        self, dbus_con, app_id, id, notification_in, notification_expected
    ):
        mock_notification = self.add_notification(dbus_con, app_id, id, notification_in)
        assert (
            mock_notification == GLib.Variant("a{sv}", notification_expected).unpack()
        )

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Notification", 2)

    def test_basic(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            NOTIFICATION_BASIC,
            NOTIFICATION_BASIC,
        )

    def test_remove(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id

        notification_intf = NotificationPortal()
        mock_intf = xdp.get_mock_iface(dbus_con)

        id = "test1"

        notification_intf.AddNotification(id, NOTIFICATION_BASIC)
        method_calls = mock_intf.GetMethodCalls("AddNotification")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[0] == app_id
        assert args[1] == id

        notification_intf.RemoveNotification(id)
        method_calls = mock_intf.GetMethodCalls("RemoveNotification")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[0] == app_id
        assert args[1] == id

    def test_buttons(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            NOTIFICATION_BUTTONS,
            NOTIFICATION_BUTTONS,
        )

    def test_markup(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id

        bodies = [
            (
                "test <b>notification</b> body <i>italic</i>",
                "test <b>notification</b> body <i>italic</i>",
            ),
            (
                'test <a href="https://example.com"><b>Some link</b></a>',
                'test <a href="https://example.com"><b>Some link</b></a>',
            ),
            (
                "&lt;html&gt;",
                "&lt;html&gt;",
            ),
            (
                '<a href="https://xkcd.com/327/#&quot;&gt;&lt;html&gt;"></a>',
                '<a href="https://xkcd.com/327/#&quot;&gt;&lt;html&gt;"></a>',
            ),
            (
                "test \n newline \n\n some more space \n  with trailing space ",
                "test newline some more space with trailing space",
            ),
            (
                "test <custom> tag </custom>",
                "test tag",
            ),
            (
                "test <b>notification<b> body",
                False,
            ),
            (
                "<b>foo<i>bar</b></i>",
                False,
            ),
            (
                "test <markup><i>notification</i><markup> body",
                False,
            ),
        ]

        i = 0
        for body_in, body_expected in bodies:
            notification_in = NOTIFICATION_BASIC.copy()
            notification_in["markup-body"] = GLib.Variant("s", body_in)

            notification_expected = NOTIFICATION_BASIC.copy()
            if body_expected:
                notification_expected["markup-body"] = GLib.Variant("s", body_expected)

            try:
                self.check_notification(
                    dbus_con,
                    app_id,
                    f"test{i}",
                    notification_in,
                    notification_expected,
                )
                assert body_expected
            except GLib.GError as e:
                assert "invalid markup-body" in e.message

            i += 1

    def test_bad_arg(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        notification = NOTIFICATION_BASIC.copy()
        notification["bodx"] = GLib.Variant("s", "Xtest")

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            NOTIFICATION_BASIC,
        )

    def test_bad_priority(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        notification = NOTIFICATION_BASIC.copy()
        notification["priority"] = GLib.Variant("s", "invalid")

        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "invalid not a priority" in e.message

    def test_bad_button(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        notification = NOTIFICATION_BUTTONS.copy()
        notification["buttons"] = GLib.Variant(
            "aa{sv}",
            [
                {
                    "labex": GLib.Variant("s", "button1"),
                    "action": GLib.Variant("s", "action1"),
                },
            ],
        )

        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "invalid button" in e.message

    def test_display_hint(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        notification = NOTIFICATION_BASIC.copy()
        notification["display-hint"] = GLib.Variant(
            "as",
            [
                "transient",
                "show-as-new",
            ],
        )

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = NOTIFICATION_BASIC.copy()
        notification["display-hint"] = GLib.Variant(
            "as",
            [
                "unsupported-hint",
            ],
        )

        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "not a display-hint" in e.message

    def test_category(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        notification = NOTIFICATION_BASIC.copy()
        notification["category"] = GLib.Variant("s", "im.received")

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = NOTIFICATION_BASIC.copy()
        notification["category"] = GLib.Variant("s", "x-vendor.custom")

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = NOTIFICATION_BASIC.copy()
        notification["category"] = GLib.Variant("s", "unsupported-type")

        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "not a supported category" in e.message

    def test_supported_options(self, portals, dbus_con):
        properties_intf = xdp.get_iface(dbus_con, "org.freedesktop.DBus.Properties")

        options = properties_intf.Get(
            "org.freedesktop.portal.Notification", "SupportedOptions"
        )

        assert options == SUPPORTED_OPTIONS

    def test_icon_themed(self, portals, dbus_con):
        notification_intf = NotificationPortal()
        icon = Gio.ThemedIcon.new("test-icon-symbolic")

        notification = NOTIFICATION_BASIC.copy()
        notification["icon"] = icon.serialize()

        notification_intf.AddNotification("test1", notification)

    def test_icon_bytes(self, portals, dbus_con):
        notification_intf = NotificationPortal()
        bytes = GLib.Bytes.new(SVG_IMAGE_DATA.encode("utf-8"))
        icon = Gio.BytesIcon.new(bytes)

        notification = NOTIFICATION_BASIC.copy()
        notification["icon"] = icon.serialize()

        notification_intf.AddNotification("test1", notification)

    def test_icon_file(self, portals, dbus_con):
        notification_intf = NotificationPortal()
        fd, file_path = tempfile.mkstemp(prefix="notification_icon_", dir=Path.home())
        os.write(fd, SVG_IMAGE_DATA.encode("utf-8"))

        file = Gio.File.new_for_path(file_path)
        icon = Gio.FileIcon.new(file)

        notification = NOTIFICATION_BASIC.copy()
        notification["icon"] = icon.serialize()

        notification = {
            "title": GLib.Variant("s", "title"),
            "icon": icon.serialize(),
        }

        notification_intf.AddNotification("test1", notification)

    def test_icon_bad(self, portals, dbus_con):
        notification_intf = NotificationPortal()

        notification = NOTIFICATION_BASIC.copy()

        bad_icons = [
            GLib.Variant("(sv)", ["themed", GLib.Variant("s", "test-icon-symbolic")]),
            GLib.Variant(
                "(sv)",
                ["bytes", GLib.Variant("as", ["test-icon-symbolic", "test-icon"])],
            ),
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("s", "")]),
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("h", 0)]),
        ]

        for icon in bad_icons:
            notification["icon"] = icon
            try:
                notification_intf.AddNotification("test1", notification)
                assert False, "This statement should not be reached"
            except GLib.GError as e:
                assert e.matches(Gio.io_error_quark(), Gio.IOErrorEnum.DBUS_ERROR)

    def test_sound_simple(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        notification = NOTIFICATION_BASIC.copy()
        notification["sound"] = GLib.Variant("s", "default")

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = NOTIFICATION_BASIC.copy()
        notification["sound"] = GLib.Variant("s", "silent")

        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = NOTIFICATION_BASIC.copy()
        notification["sound"] = GLib.Variant("s", "bad")

        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "invalid sound: invalid option" in e.message

    def test_sound_file(self, portals, dbus_con, xdp_app_info):
        fd, file_path = tempfile.mkstemp(prefix="notification_sound_", dir=Path.home())
        os.write(fd, SOUND_DATA)

        file = Gio.File.new_for_path(file_path)

        notification = NOTIFICATION_BASIC.copy()
        notification["sound"] = GLib.Variant(
            "(sv)",
            (
                "file",
                GLib.Variant("s", file.get_uri()),
            ),
        )

        mock_notification = self.add_notification(
            dbus_con,
            xdp_app_info.app_id,
            "test1",
            notification,
        )

        assert "sound" not in mock_notification

    def test_sound_fd(self, portals, dbus_con, xdp_app_info):
        fd = os.memfd_create("notification_sound_test", os.MFD_ALLOW_SEALING)
        os.write(fd, SOUND_DATA)

        notification = NOTIFICATION_BASIC.copy()
        notification["sound"] = GLib.Variant(
            "(sv)",
            (
                "file-descriptor",
                GLib.Variant("h", 0),
            ),
        )

        mock_notification = self.add_notification(
            dbus_con,
            xdp_app_info.app_id,
            "test1",
            notification,
            [fd],
        )

        assert mock_notification["sound"][0] == "file-descriptor"
        mock_fd = mock_notification["sound"][1]
        mock_fd = mock_fd.take()

        os.lseek(fd, 0, os.SEEK_SET)
        fd_contents = os.read(mock_fd, 1000)
        assert fd_contents == SOUND_DATA

        os.close(mock_fd)
        os.close(fd)

    def test_sound_bad(self, portals, dbus_con):
        notification_intf = NotificationPortal()

        notification = NOTIFICATION_BASIC.copy()

        bad_sounds = [
            # bad type
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("s", "")]),
            # not sending the FD for the handle
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("h", 13)]),
        ]

        for sound in bad_sounds:
            notification["sound"] = sound
            try:
                notification_intf.AddNotification("test1", notification)
                assert False, "This statement should not be reached"
            except GLib.GError as e:
                assert e.matches(Gio.io_error_quark(), Gio.IOErrorEnum.DBUS_ERROR)
                pass
