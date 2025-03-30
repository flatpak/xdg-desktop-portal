# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import Response, init_logger, ImplRequest

import dbus
import dbus.service
from dataclasses import dataclass


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


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "wallpaper_params")
    mock.wallpaper_params = WallpaperParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osssa{sv}",
    out_signature="u",
    async_callbacks=("cb_success", "cb_error"),
)
def SetWallpaperURI(
    self, handle, app_id, parent_window, uri, options, cb_success, cb_error
):
    logger.debug(
        f"SetWallpaperURI({handle}, {app_id}, {parent_window}, {uri}, {options})"
    )
    params = self.wallpaper_params

    # This is irregular: the backend doesn't return the results vardict
    def internal_cb_success(response, results):
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
