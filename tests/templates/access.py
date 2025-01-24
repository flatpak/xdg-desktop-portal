# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_logger, ImplRequest

import dbus.service
from gi.repository import GLib


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Access"


logger = init_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osssssa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def AccessDialog(
    self,
    handle,
    app_id,
    parent_window,
    title,
    subtitle,
    body,
    options,
    cb_success,
    cb_error,
):
    try:
        logger.debug(
            f"AccessDialog({handle}, {app_id}, {parent_window}, {title}, {subtitle}, {body}, {options})"
        )

        def closed_callback():
            response = Response(2, {})
            logger.debug(f"AccessDialog Close() response {response}")
            cb_success(response.response, response.results)

        def reply_callback(request):
            response = Response(self.response, {})
            logger.debug(f"AccessDialog with response {response}")
            request.unexport()
            cb_success(response.response, response.results)

        request = ImplRequest(self, BUS_NAME, handle)
        if self.expect_close:
            request.export(closed_callback)
        else:
            request.export()

            logger.debug(f"scheduling delay of {self.delay}")
            GLib.timeout_add(self.delay, reply_callback, request)
    except Exception as e:
        logger.critical(e)
        cb_error(e)
