# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import dbus.service
import dbusmock
from gi.repository import GLib

from tests.templates.xdp_utils import init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Notification"
VERSION = 2


logger = init_logger(__name__)


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
                "SupportedOptions": dbus.Dictionary(
                    parameters.get("SupportedOptions", {}), signature="sv"
                ),
            },
        ),
    )
    mock.notifications = {}


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="ssa{sv}",
    out_signature="",
)
def AddNotification(  # type: ignore [no-untyped-def]
    self, app_id: str, id: str, notification: list[dict[str, GLib.Variant]]
) -> None:
    logger.debug(f"AddNotification({app_id}, {id}, {notification})")

    self.notifications.setdefault(app_id, {})[id] = notification


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="ss",
    out_signature="",
)
def RemoveNotification(self, app_id: str, id: str) -> None:  # type: ignore [no-untyped-def]
    logger.debug(f"AddNotification({app_id}, {id})")

    del self.notifications[app_id][id]


@dbus.service.method(  # type: ignore [untyped-decorator]
    dbusmock.MOCK_IFACE,
    in_signature="sssav",
    out_signature="",
)
def EmitActionInvoked(  # type: ignore [no-untyped-def]
    self, app_id: str, id: str, action: str, parameter: list[GLib.Variant]
) -> None:
    logger.debug(f"EmitActionInvoked({app_id}, {id}, {action}, {parameter})")

    # n = self.notifications[app_id][id]
    # FIXME check action is in n

    self.EmitSignal(
        MAIN_IFACE,
        "ActionInvoked",
        "sssav",
        [
            app_id,
            id,
            action,
            dbus.Array(parameter, signature="v"),
        ],
    )
