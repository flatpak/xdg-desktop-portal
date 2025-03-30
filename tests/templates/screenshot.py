# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import Response, init_logger, ImplRequest

import dbus.service
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Screenshot"
VERSION = 2


logger = init_logger(__name__)


@dataclass
class ScreenshotParameters:
    delay: int
    response: int
    results: dict
    expect_close: bool


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "screenshot_params")
    mock.screenshot_params = ScreenshotParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        results=parameters.get("results", {}),
        expect_close=parameters.get("expect-close", False),
    )

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
    logger.debug(f"Screenshot({handle}, {app_id}, {parent_window}, {options})")
    params = self.screenshot_params

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(params.response, params.results), delay=params.delay)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def PickColor(self, handle, app_id, parent_window, options, cb_success, cb_error):
    logger.debug(f"PickColor({handle}, {app_id}, {parent_window}, {options})")
    params = self.screenshot_params

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(params.response, params.results), delay=params.delay)
