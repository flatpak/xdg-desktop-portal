# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_template_logger, ImplRequest
import dbus.service

from gi.repository import GLib


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Email"
VERSION = 3


logger = init_template_logger(__name__)


def load(mock, parameters=None):
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
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def ComposeEmail(self, handle, app_id, parent_window, options, cb_success, cb_error):
    try:
        logger.debug(f"ComposeEmail({handle}, {app_id}, {parent_window}, {options})")

        response = Response(self.response, {})

        request = ImplRequest(self, BUS_NAME, handle)

        if self.expect_close:

            def closed_callback():
                response = Response(2, {})
                logger.debug(f"ComposeEmail Close() response {response}")
                cb_success(response.response, response.results)

            request.export(closed_callback)
        else:
            request.export()

            def reply():
                logger.debug(f"ComposeEmail with response {response}")
                cb_success(response.response, response.results)

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply)
    except Exception as e:
        logger.critical(e)
        cb_error(e)
