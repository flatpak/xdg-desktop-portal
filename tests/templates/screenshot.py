# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_template_logger, ImplRequest

import dbus.service
from gi.repository import GLib


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Screenshot"
VERSION = 2


logger = init_template_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.results = parameters.get("results", {})
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
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def Screenshot(self, handle, app_id, parent_window, options, cb_success, cb_error):
    try:
        logger.debug(f"Screenshot({handle}, {app_id}, {parent_window}, {options})")

        def closed_callback():
            response = Response(2, {})
            logger.debug(f"Screenshot Close() response {response}")
            cb_success(response.response, response.results)

        def reply_callback():
            response = Response(self.response, self.results)
            logger.debug(f"Screenshot with response {response}")
            cb_success(response.response, response.results)

        request = ImplRequest(self, BUS_NAME, handle)
        if self.expect_close:
            request.export(closed_callback)
        else:
            request.export()

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply_callback)
    except Exception as e:
        logger.critical(e)
        cb_error(e)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def PickColor(self, handle, app_id, parent_window, options, cb_success, cb_error):
    try:
        logger.debug(f"PickColor({handle}, {app_id}, {parent_window}, {options})")

        def closed_callback():
            response = Response(2, {})
            logger.debug(f"PickColor Close() response {response}")
            cb_success(response.response, response.results)

        def reply_callback():
            response = Response(self.response, self.results)
            logger.debug(f"PickColor with response {response}")
            cb_success(response.response, response.results)

        request = ImplRequest(self, BUS_NAME, handle)
        if self.expect_close:
            request.export(closed_callback)
        else:
            request.export()

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply_callback)
    except Exception as e:
        logger.critical(e)
        cb_error(e)
