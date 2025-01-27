# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_logger, ImplRequest
import dbus.service


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.DynamicLauncher"
VERSION = 1


logger = init_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )
    mock.launcher_name: str = parameters.get("launcher-name", None)


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

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    response = Response(
        self.response,
        {
            "name": self.launcher_name if self.launcher_name else name,
            "icon": dbus.Struct(list(icon_v), signature="sv", variant_level=2),
        },
    )

    if self.expect_close:
        request.wait_for_close()
    else:
        request.respond(response, delay=self.delay)
