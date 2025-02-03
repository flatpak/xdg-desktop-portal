# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_logger, ImplRequest
import dbus
import dbus.service
from dbusmock import MOCK_IFACE


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Usb"
VERSION = 1


logger = init_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.expect_close: bool = parameters.get("expect-close", False)
    mock.filters = parameters.get("filters", {})
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
    in_signature="ossa(sa{sv}a{sv})a{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def AcquireDevices(
    self,
    handle,
    parent_window,
    app_id,
    devices,
    options,
    cb_success,
    cb_error,
):
    logger.debug(
        f"AcquireDevices({handle}, {parent_window}, {app_id}, {devices}, {options})"
    )

    request = ImplRequest(
        self,
        BUS_NAME,
        handle,
        logger,
        cb_success,
        cb_error,
    )

    def reply():
        # no options supported
        assert not options
        devices_out = []

        for device in devices:
            (id, info, access_options) = device
            props = info["properties"]

            allows_writable = self.filters.get("writable", True)
            needs_writable = access_options.get("writable", False)
            if needs_writable and not allows_writable:
                logger.debug(f"Skipping device {id} because it requires writable")
                continue

            needs_vendor = self.filters.get("vendor", None)
            needs_vendor = int(needs_vendor, 16) if needs_vendor else None

            vendor = props.get("ID_VENDOR_ID", None)
            vendor = int(vendor, 16) if vendor else None

            if needs_vendor is not None and needs_vendor != vendor:
                logger.debug(
                    f"Skipping device {id} because it does not belong to vendor {needs_vendor:02x}"
                )
                continue

            needs_model = self.filters.get("model", None)
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
            self.response,
            {"devices": dbus.Array(devices_out, signature="(sa{sv})", variant_level=1)},
        )

    if self.expect_close:
        request.wait_for_close()
    else:
        request.respond(reply, delay=self.delay)


@dbus.service.method(
    MOCK_IFACE,
    in_signature="a{sv}",
    out_signature="",
)
def SetSelectionFilters(self, filters):
    logger.debug(f"SetSelectionFilters({filters})")

    self.filters = filters
