# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import init_template_logger
import dbus.service
import dbus
from gi.repository import GLib

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Background"
VERSION = 1

logger = init_template_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="",
    out_signature="a{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def GetAppState(self, cb_success, cb_error):
    logger.debug("GetAppState()")

    # FIXME: implement?
    def reply():
        cb_success({})

    logger.debug(f"scheduling delay of {self.delay}")
    GLib.timeout_add(self.delay, reply)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oss",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def NotifyBackground(self, handle, app_id, name, cb_success, cb_error):
    logger.debug(f"NotifyBackground({handle}, {app_id}, {name})")

    logger.debug(f"scheduling delay of {self.delay}")
    GLib.timeout_add(self.delay, cb_success)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="sbasu",
    out_signature="b",
)
def EnableAutostart(self, app_id, enable, commandline, flags):
    raise dbus.exceptions.DBusException("EnableAutostart is deprecated")
