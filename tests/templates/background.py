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

from tests.templates.xdp_utils import AsyncCallback, init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Background"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class BackgroundParameters:
    delay: int


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "background_params")
    mock.background_params = BackgroundParameters(
        delay=parameters.get("delay", 200),
    )


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="",
    out_signature="a{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def GetAppState(self, cb_success: AsyncCallback, cb_error: AsyncCallback) -> None:  # type: ignore [no-untyped-def]
    logger.debug("GetAppState()")
    params = self.background_params

    # FIXME: implement?
    def reply() -> None:
        cb_success({})

    logger.debug(f"scheduling delay of {params.delay}")
    GLib.timeout_add(params.delay, reply)


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="oss",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def NotifyBackground(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    app_id: str,
    name: str,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
    logger.debug(f"NotifyBackground({handle}, {app_id}, {name})")
    params = self.background_params

    logger.debug(f"scheduling delay of {params.delay}")
    GLib.timeout_add(params.delay, cb_success)


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="sbasu",
    out_signature="b",
)
def EnableAutostart(  # type: ignore [no-untyped-def]
    self, app_id: str, enable: bool, commandline: Any, flags: Any
) -> None:
    raise dbus.exceptions.DBusException("EnableAutostart is deprecated")
