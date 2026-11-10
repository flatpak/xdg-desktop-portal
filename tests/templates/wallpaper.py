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
MAIN_IFACE = "org.freedesktop.impl.portal.Wallpaper"


logger = init_logger(__name__)


@dataclass
class WallpaperParameters:
    delay: int
    response: int
    expect_close: bool


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "wallpaper_params")
    mock.wallpaper_params = WallpaperParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
    )


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="osssa{sv}",
    out_signature="u",
    async_callbacks=("cb_success", "cb_error"),
)
def SetWallpaperURI(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    app_id: str,
    parent_window: Any,
    uri: str,
    options: ASV,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
):
    logger.debug(
        f"SetWallpaperURI({handle}, {app_id}, {parent_window}, {uri}, {options})"
    )
    params = self.wallpaper_params

    # This is irregular: the backend doesn't return the results vardict
    def internal_cb_success(response: Response, results: ASV) -> None:
        cb_success(response)

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        internal_cb_success,
        cb_error,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(params.response, {}), delay=params.delay)
