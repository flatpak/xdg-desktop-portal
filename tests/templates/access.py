# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from dataclasses import dataclass
from typing import Any

import dbus.service
import dbusmock
from gi.repository import GLib

from tests.templates.xdp_utils import AsyncCallback, ImplRequest, Response, init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Access"


logger = init_logger(__name__)


@dataclass
class AccessParameters:
    delay: int
    response: int
    expect_close: bool


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "access_params")
    mock.access_params = AccessParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
    )


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="osssssa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def AccessDialog(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    app_id: str,
    parent_window: Any | None,
    title: str,
    subtitle: str,
    body: str,
    options: dbus.Dictionary,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
    logger.debug(
        f"AccessDialog({handle}, {app_id}, {parent_window}, {title}, {subtitle}, {body}, {options})"
    )
    params = self.access_params

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
