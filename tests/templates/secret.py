# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import os
from dataclasses import dataclass

import dbus
import dbus.service

from tests.templates.xdp_utils import ImplRequest, Response, init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Secret"


logger = init_logger(__name__)


@dataclass
class SecretParameters:
    delay: int
    response: int
    secret: bytes
    token: str
    expect_close: bool


def load(mock, parameters=None):
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "secret_params")
    mock.secret_params = SecretParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        secret=parameters.get("secret", b"a-master-secret"),
        token=parameters.get("token", "a-token"),
        expect_close=parameters.get("expect-close", False),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osha{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def RetrieveSecret(self, handle, app_id, fd, options, cb_success, cb_error):
    logger.debug(f"RetrieveSecret({handle}, {app_id}, {fd}, {options})")
    params = self.secret_params

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
        return

    results = {}

    # The fd is ours once it has been passed, so it is closed either way; that
    # close is what ends the client's read
    with os.fdopen(fd.take(), "wb") as out:
        if params.response == 0:
            out.write(params.secret)

    if params.response == 0 and params.token is not None:
        results["token"] = dbus.String(params.token, variant_level=1)

    request.respond(Response(params.response, results), delay=params.delay)
