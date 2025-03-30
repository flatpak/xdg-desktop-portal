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
MAIN_IFACE = "org.freedesktop.impl.portal.DynamicLauncher"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class DynamiclauncherParameters:
    delay: int
    response: int
    expect_close: bool
    launcher_name: str


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "dynamiclauncher_params")
    mock.dynamiclauncher_params = DynamiclauncherParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        launcher_name=parameters.get("launcher-name", None),
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
    in_signature="osssva{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def PrepareInstall(
    self, handle, app_id, parent_window, name, icon_v, options, cb_success, cb_error
):
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
