# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from dataclasses import dataclass

import dbus
import dbus.service

from tests.templates.xdp_utils import ImplRequest, Response, init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Account"


logger = init_logger(__name__)


@dataclass
class AccountParameters:
    delay: int
    response: int
    results: dict
    expect_close: bool
    request_version: int


def load(mock, parameters=None):
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "account_params")
    mock.account_params = AccountParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        results=parameters.get("results", {}),
        expect_close=parameters.get("expect-close", False),
        request_version=parameters.get("request-version", 1),
    )

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "request-version": dbus.UInt32(mock.account_params.request_version),
            }
        ),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def GetUserInformation(self, handle, app_id, window, options, cb_success, cb_error):
    logger.debug(f"GetUserInformation({handle}, {app_id}, {window}, {options})")
    params = self.account_params

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
        request_version=params.request_version,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(params.response, params.results), delay=params.delay)
