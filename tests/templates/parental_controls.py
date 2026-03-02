# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import Response, init_logger, ImplRequest

import dbus.service
import dbus
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.ParentalControls"


logger = init_logger(__name__)


@dataclass
class ParentalControlsParameters:
    delay: int
    response: int
    results: dict
    expect_close: bool


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "parental_controls_params")
    mock.parental_controls_params = ParentalControlsParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        results=parameters.get("results", {}),
        expect_close=parameters.get("expect-close", False),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossaua{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def QueryAgeBracket(self, handle, app_id, window, gates, options, cb_success, cb_error):
    logger.debug(f"QueryAgeBracket({handle}, {app_id}, {window}, {gates}, {options})")
    params = self.parental_controls_params

    # Default to no information known
    results = (
        params.results
        if params.results
        else {"low": dbus.Int32(-1), "high": dbus.Int32(-1)}
    )

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
        request.respond(Response(params.response, results), delay=params.delay)

