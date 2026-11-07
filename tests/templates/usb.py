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
MAIN_IFACE = "org.freedesktop.impl.portal.Usb"
VERSION = 1


logger = init_logger(__name__)


@dataclass
class UsbParameters:
    delay: int
    response: int
    expect_close: bool
    filters: dict[str, Any]


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}

    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "usb_params")

    mock.usb_params = UsbParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
        expect_close=parameters.get("expect-close", False),
        filters=parameters.get("filters", {}),
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
    in_signature="ossa(sa{sv}a{sv})a{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def AcquireDevices(  # type: ignore [no-untyped-def]
    self,
    handle: dbus.ObjectPath,
    parent_window: Any,
    app_id: str,
    devices: list[tuple[int, dict[str, Any]]],
    options: ASV,
    cb_success: AsyncCallback,
    cb_error: AsyncCallback,
) -> None:
    logger.debug(
        f"AcquireDevices({handle}, {parent_window}, {app_id}, {devices}, {options})"
    )
    params = self.usb_params

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    def reply() -> Response:
        # no options supported
        assert not options
        devices_out = []

        for device in devices:
            (id, info, access_options) = device
            props = info["properties"]

            allows_writable = params.filters.get("writable", True)
            needs_writable = access_options.get("writable", False)
            if needs_writable and not allows_writable:
                logger.debug(f"Skipping device {id} because it requires writable")
                continue

            needs_vendor = params.filters.get("vendor", None)
            needs_vendor = int(needs_vendor, 16) if needs_vendor else None

            vendor = props.get("ID_VENDOR_ID", None)
            vendor = int(vendor, 16) if vendor else None

            if needs_vendor is not None and needs_vendor != vendor:
                logger.debug(
                    f"Skipping device {id} because it does not belong to vendor {needs_vendor:02x}"
                )
                continue

            needs_model = params.filters.get("model", None)
            needs_model = int(needs_model, 16) if needs_model else None

            model = props.get("ID_MODEL_ID", None)
            model = int(model, 16) if model else None

            if needs_model is not None and needs_model != model:
                logger.debug(
                    f"Skipping device {id} because it is not a model {needs_model:02x}"
                )
                continue

            devices_out.append(
                dbus.Struct([id, access_options], signature="sa{sv}", variant_level=1)
            )

        return Response(
            params.response,
            {"devices": dbus.Array(devices_out, signature="(sa{sv})", variant_level=1)},
        )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(reply, delay=params.delay)


@dbus.service.method(  # type: ignore [untyped-decorator]
    dbusmock.MOCK_IFACE,
    in_signature="a{sv}",
    out_signature="",
)
def SetSelectionFilters(self, filters: dict[str, GLib.Variant]) -> None:  # type: ignore [no-untyped-def]
    logger.debug(f"SetSelectionFilters({filters})")
    params = self.usb_params

    params.filters = filters
