# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import dbus
import dbus.service

from tests.templates.xdp_utils import init_logger

# The WirePlumber plugin's service
BUS_NAME = "org.freedesktop.impl.portal.AppAudioCapture"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.AppAudioCapture"
SESSION_IFACE = "org.freedesktop.impl.portal.Session"


logger = init_logger(__name__)


def load(mock, parameters=None):
    logger.debug(f"Loading parameters: {parameters}")

    mock.AddProperties(MAIN_IFACE, dbus.Dictionary({"version": dbus.UInt32(1)}))
    mock.n_created = 0


def create_stream(self, session_handle, options):
    n = self.n_created
    self.n_created += 1
    return dbus.Dictionary(
        {
            "pipewire-node-id": dbus.UInt32(77 + n),
            "pipewire-serial": dbus.UInt64(7777 + n),
        },
        signature="sv",
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="",
)
def CreateSession(self, handle, app_id, options):
    logger.debug(f"CreateSession({handle}, {app_id}, {options})")
    self.AddObject(handle, SESSION_IFACE, {}, [("Close", "", "", "")])


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oa{sv}",
    out_signature="a{sv}",
)
def CreateSystemStream(self, session_handle, options):
    logger.debug(f"CreateSystemStream({session_handle}, {options})")
    return create_stream(self, session_handle, options)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oa{sv}",
    out_signature="a{sv}",
)
def CreateApplicationStream(self, session_handle, options):
    logger.debug(f"CreateApplicationStream({session_handle}, {options})")
    return create_stream(self, session_handle, options)
