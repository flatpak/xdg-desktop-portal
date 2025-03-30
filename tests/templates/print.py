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
MAIN_IFACE = "org.freedesktop.impl.portal.Print"
VERSION = 3


logger = init_logger(__name__)


@dataclass
class PrintParameters:
    delay: int
    response: int
    results: dict
    expect_close: bool
    prepare_results: dict


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "print_params")
    mock.print_params = PrintParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        results=parameters.get("results", {}),
        expect_close=parameters.get("expect-close", False),
        prepare_results=parameters.get("prepare-results", {}),
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
    in_signature="osssa{sv}a{sv}a{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def PreparePrint(
    self,
    handle,
    app_id,
    parent_window,
    title,
    settings,
    page_setup,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"PreparePrint({handle}, {app_id}, {parent_window}, {title}, {settings}, {page_setup}, {options})"
    )
    params = self.print_params

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
        request.respond(
            Response(params.response, params.prepare_results), delay=params.delay
        )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osssha{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def Print(
    self, handle, app_id, parent_window, title, fd, options, cb_success, cb_error
):
    logger.debug(
        f"Print({handle}, {app_id}, {parent_window}, {title}, {fd}, {options})"
    )
    params = self.print_params

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
