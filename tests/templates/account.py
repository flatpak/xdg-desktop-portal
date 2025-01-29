# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates import Response, init_logger, ImplRequest

import dbus.service
import dbus
from dataclasses import dataclass


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


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "account_params")
    mock.account_params = AccountParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        results=parameters.get("results", {}),
        expect_close=parameters.get("expect-close", False),
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
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(params.response, params.results), delay=params.delay)
