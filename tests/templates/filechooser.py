# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates import Response, init_logger, ImplRequest

import dbus.service
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.desktop.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.FileChooser"
VERSION = 4


logger = init_logger(__name__)


@dataclass
class FilechooserParameters:
    delay: int
    response: int
    results: dict
    expect_close: bool


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "filechooser_params")
    mock.filechooser_params = FilechooserParameters(
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
    in_signature="osssa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def OpenFile(self, handle, app_id, parent_window, title, options, cb_success, cb_error):
    logger.debug(f"OpenFile({handle}, {app_id}, {parent_window}, {title}, {options})")
    params = self.filechooser_params

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
    in_signature="osssa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def SaveFile(self, handle, app_id, parent_window, title, options, cb_success, cb_error):
    logger.debug(f"SaveFile({handle}, {app_id}, {parent_window}, {title}, {options})")
    params = self.filechooser_params

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
