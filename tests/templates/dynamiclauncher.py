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
MAIN_IFACE = "org.freedesktop.impl.portal.DynamicLauncher"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class DynamiclauncherParameters:
    delay: int
    response: int
    expect_close: bool
    launcher_name: str


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "dynamiclauncher_params")
    mock.dynamiclauncher_params = DynamiclauncherParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        launcher_name=parameters.get("launcher-name", ""),
    )

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="osssva{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def PrepareInstall(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    app_id: str,
    parent_window: Any,
    name: str,
    icon_v: dbus.Variant,
    options: list[str],
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
    logger.debug(
        f"PrepareInstall({handle}, {app_id}, {parent_window}, {name}, {icon_v}, {options})"
    )
    params = self.dynamiclauncher_params

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    response = Response(
        params.response,
        {
            "name": params.launcher_name if params.launcher_name else name,
            "icon": dbus.Struct(list(icon_v), signature="sv", variant_level=2),
        },
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(response, delay=params.delay)
