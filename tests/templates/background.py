# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates import init_logger

import dbus.service
import dbus
from gi.repository import GLib
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.desktop.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Background"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class BackgroundParameters:
    delay: int


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "background_params")
    mock.background_params = BackgroundParameters(
        delay=parameters.get("delay", 200),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="",
    out_signature="a{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def GetAppState(self, cb_success, cb_error):
    logger.debug("GetAppState()")
    params = self.background_params

    # FIXME: implement?
    def reply():
        cb_success({})

    logger.debug(f"scheduling delay of {params.delay}")
    GLib.timeout_add(params.delay, reply)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oss",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def NotifyBackground(self, handle, app_id, name, cb_success, cb_error):
    logger.debug(f"NotifyBackground({handle}, {app_id}, {name})")
    params = self.background_params

    logger.debug(f"scheduling delay of {params.delay}")
    GLib.timeout_add(params.delay, cb_success)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="sbasu",
    out_signature="b",
)
def EnableAutostart(self, app_id, enable, commandline, flags):
    raise dbus.exceptions.DBusException("EnableAutostart is deprecated")
