# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from dataclasses import dataclass
from typing import Any

import dbus
import dbus.service
import dbusmock
from gi.repository import GLib

from tests.templates.xdp_utils import AsyncCallback, ImplRequest, Response, init_logger
from tests.xdp_utils import ASV

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Screenshot"
VERSION = 3
AVAILABLE_TARGETS = 1 | 2 | 4 | 8


logger = init_logger(__name__)


@dataclass
class ScreenshotParameters:
    delay: int
    response: int
    results: dict[str, Any]
    expect_close: bool


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

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
                "AvailableTargets": dbus.UInt32(
                    parameters.get("available-targets", AVAILABLE_TARGETS)
                ),
            }
        ),
    )


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def Screenshot(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    app_id: str,
    parent_window: Any,
    options: ASV,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
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


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def PickColor(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    app_id: str,
    parent_window: Any,
    options: ASV,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
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
