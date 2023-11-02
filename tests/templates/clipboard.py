# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import init_template_logger
import dbus.service
import dbus
import tempfile

from gi.repository import GLib

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Clipboard"
VERSION = 1

logger = init_template_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oa{sv}",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def RequestClipboard(self, session_handle, options, cb_success, cb_error):
    try:
        logger.debug(f"RequestClipboard({session_handle}, {options})")

        if self.expect_close:
            cb_success()
        else:
            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, cb_success)
    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oa{sv}",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def SetSelection(self, session_handle, options, cb_success, cb_error):
    try:
        logger.debug(f"SetSelection({session_handle}, {options})")

        if self.expect_close:
            cb_success()
        else:
            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, cb_success)
    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ou",
    out_signature="h",
    async_callbacks=("cb_success", "cb_error"),
)
def SelectionWrite(self, session_handle, serial, cb_success, cb_error):
    try:
        logger.debug(f"SelectionWrite({session_handle}, {serial})")

        temp_file = tempfile.TemporaryFile()
        fd = dbus.types.UnixFd(temp_file.fileno())

        if self.expect_close:
            cb_success(fd)
        else:

            def reply():
                cb_success(fd)

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply)
    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oub",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def SelectionWriteDone(self, session_handle, serial, success, cb_success, cb_error):
    try:
        logger.debug(f"SelectionWriteDone({session_handle}, {serial}, {success})")

        if self.expect_close:
            cb_success()
        else:
            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, cb_success)
    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="os",
    out_signature="h",
    async_callbacks=("cb_success", "cb_error"),
)
def SelectionRead(self, session_handle, mime_type, cb_success, cb_error):
    try:
        logger.debug(f"SelectionRead({session_handle}, {mime_type})")

        temp_file = tempfile.TemporaryFile()
        fd = dbus.types.UnixFd(temp_file.fileno())

        if self.expect_close:
            cb_success(fd)
        else:

            def reply():
                cb_success(fd)

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply)
    except Exception as e:
        logger.critical(e)
        cb_error(e)
