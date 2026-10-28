# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import time
from dataclasses import dataclass
from typing import Any

import dbus
import dbus.service
import dbusmock
from gi.repository import GLib

from tests.templates.xdp_utils import (
    AsyncCallback,
    ImplRequest,
    ImplSession,
    Response,
    init_logger,
)
from tests.xdp_utils import ASV

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.GlobalShortcuts"
VERSION = 2

type Shortcuts = dict[str, dict[str, GLib.Variant]]


logger = init_logger(__name__)


@dataclass
class GlobalshortcutsParameters:
    delay: int
    response: int
    expect_close: bool
    force_close: int


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "globalshortcuts_params")
    mock.globalshortcuts_params = GlobalshortcutsParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        force_close=parameters.get("force-close", 0),
    )

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )
    mock.sessions: dict[str, ImplSession] = {}


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateSession(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    session_handle: dbus.ObjectPath,
    app_id: str,
    options: ASV,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
    logger.debug(f"CreateSession({handle}, {session_handle}, {app_id}, {options})")
    params = self.globalshortcuts_params

    session = ImplSession(self, BUS_NAME, session_handle, app_id).export()
    self.sessions[session_handle] = session

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
        request.respond(
            Response(params.response, {"session_handle": session.handle}),
            delay=params.delay,
        )
        if params.force_close > 0:
            GLib.timeout_add(params.force_close, session.close)


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="ooa(sa{sv})sa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def BindShortcuts(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    session_handle: dbus.ObjectPath,
    shortcuts: Shortcuts,
    parent_window: Any,
    options: ASV,
    cb_success,
    cb_error,
):
    logger.debug(f"BindShortcuts({handle}, {session_handle}, {shortcuts}, {options})")
    params = self.globalshortcuts_params

    assert session_handle in self.sessions

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

        def reply() -> Response:
            logger.debug(f"BindShortcuts with shortcuts {shortcuts}")
            self.sessions[session_handle].shortcuts = shortcuts
            return Response(params.response, {})

        request.respond(reply, delay=params.delay)


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="oo",
    out_signature="ua{sv}",
)
def ListShortcuts(  # type: ignore [no-untyped-def]
    self,
    handle,
    session_handle,
) -> tuple[int, dict[str, Shortcuts]]:
    shortcuts: Shortcuts = self.sessions[session_handle].shortcuts
    return (0, {"shortcuts": shortcuts})


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="",
)
def ConfigureShortcuts(  # type: ignore [no-untyped-def]
    self, session_handle: dbus.ObjectPath, parent_window: Any, options: ASV
) -> None:
    assert session_handle in self.sessions


@dbus.service.method(  # type: ignore [untyped-decorator]
    dbusmock.MOCK_IFACE,
    in_signature="os",
    out_signature="",
)
def Trigger(self, session_handle: dbus.ObjectPath, shortcut_id: str) -> None:  # type: ignore [no-untyped-def]
    now_since_epoch = int(time.time() * 1000000)
    self.EmitSignal(
        MAIN_IFACE,
        "Activated",
        "osta{sv}",
        [session_handle, shortcut_id, now_since_epoch, {}],
    )
    time.sleep(0.2)
    now_since_epoch = int(time.time() * 1000000)
    self.EmitSignal(
        MAIN_IFACE,
        "Deactivated",
        "osta{sv}",
        [session_handle, shortcut_id, now_since_epoch, {}],
    )
