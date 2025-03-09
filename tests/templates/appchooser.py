# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates import Response, init_logger, ImplRequest

import dbus.service
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.AppChooser"
VERSION = 2


logger = init_logger(__name__)


@dataclass
class AppchooserParameters:
    delay: int
    response: int
    expect_close: bool


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "appchooser_params")
    mock.appchooser_params = AppchooserParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
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
    in_signature="ossasa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def ChooseApplication(
    self, handle, app_id, parent_window, choices, options, cb_success, cb_error
):
    logger.debug(
        f"ChooseApplication({handle}, {app_id}, {parent_window}, {choices}, {options})"
    )
    params = self.appchooser_params

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
        request.respond(Response(params.response, {}), delay=params.delay)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oas",
    out_signature="",
)
def UpdateChoices(self, handle, choices):
    logger.debug(f"UpdateChoices({handle}, {choices})")
