# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, ImplSession
from collections import namedtuple
from itertools import count
from gi.repository import GLib

import dbus
import dbus.service
import logging
import socket

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.InputCapture"
VERSION = 2

logger = logging.getLogger(f"templates.{__name__}")
logger.setLevel(logging.DEBUG)

serials = count()

Response = namedtuple("Response", ["response", "results"])
Barrier = namedtuple("Barrier", ["id", "position"])


def load(mock, parameters=None):
    logger.debug(f"Loading parameters: {parameters}")
    # Delay before Request.response
    mock.delay: int = parameters.get("delay", 0)

    mock.supported_capabilities = parameters.get("supported_capabilities", 0xF)
    # The actual ones we reply with in the CreateSession request
    mock.capabilities = parameters.get("capabilities", None)

    mock.default_zone = parameters.get("default-zone", [(1920, 1080, 0, 0)])
    mock.current_zones = mock.default_zone
    mock.current_zone_set = next(serials)

    mock.disable_delay = parameters.get("disable-delay", 0)
    mock.activated_delay = parameters.get("activated-delay", 0)
    mock.deactivated_delay = parameters.get("deactivated-delay", 0)

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
                "SupportedCapabilities": dbus.UInt32(mock.supported_capabilities),
            }
        ),
    )

    mock.sessions: dict[str, ImplSession] = {}


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oossa{sv}",
    out_signature="ua{sv}",
)
def CreateSession(self, handle, session_handle, app_id, parent_window, options):
    try:
        logger.debug(f"CreateSession({parent_window}, {options})")

        assert "capabilities" in options

        session = ImplSession(self, BUS_NAME, session_handle).export()
        self.sessions[session_handle] = session

        # Filter to the subset of supported capabilities
        if self.capabilities is None:
            capabilities = options["capabilities"]
        else:
            capabilities = self.capabilities

        capabilities &= self.supported_capabilities

        response = Response(0, {"session_handle": session.handle})
        response.results["capabilities"] = dbus.UInt32(capabilities)

        if options.get("persist_mode") != 0:
            restore_data = options.get("restore_data")
            if not restore_data:
                # The restore data isn't actually visible to the app but oh well
                data = dbus.String("some restore token", variant_level=1)
                self.restore_data = dbus.Struct(
                    list(
                        [
                            dbus.String("TEST", variant_level=0),
                            dbus.UInt32(1, variant_level=0),
                            data,
                        ]
                    ),
                    signature="suv",
                    variant_level=0,
                )
            else:
                if restore_data != self.restore_data:
                    logger.error(f"Invalid restore_data passed")
                    return (2, {})
            response.results["restore_data"] = self.restore_data

        logger.debug(f"CreateSession with response {response}")

        return response.response, response.results
    except Exception as e:
        logger.critical(e)
        return (2, {})


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}",
    out_signature="ua{sv}",
)
def GetZones(self, handle, session_handle, app_id, options):
    try:
        logger.debug(f"GetZones({session_handle}, {options})")

        assert session_handle in self.sessions

        response = Response(0, {})
        response.results["zones"] = self.default_zone
        response.results["zone_set"] = dbus.UInt32(
            self.current_zone_set, variant_level=1
        )
        logger.debug(f"GetZones with response {response}")

        if response.response == 0:
            self.current_zones = response.results["zones"]

        return response.response, response.results
    except Exception as e:
        logger.critical(e)
        return (2, {})


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oosa{sv}aa{sv}u",
    out_signature="ua{sv}",
)
def SetPointerBarriers(
    self, handle, session_handle, app_id, options, barriers, zone_set
):
    try:
        logger.debug(
            f"SetPointerBarriers({session_handle}, {options}, {barriers}, {zone_set})"
        )

        assert session_handle in self.sessions
        assert zone_set == self.current_zone_set

        self.current_barriers = []

        failed_barriers = []

        # Barrier sanity checks:
        for b in barriers:
            id = b["barrier_id"]
            x1, y1, x2, y2 = b["position"]
            if (x1 != x2 and y1 != y2) or (x1 == x2 and y1 == y2):
                logger.debug(f"Barrier {id} is not horizontal or vertical")
                failed_barriers.append(id)
                continue

            for z in self.current_zones:
                w, h, x, y = z
                if x1 < x or x1 > x + w:
                    continue
                if y1 < y or y1 > y + h:
                    continue

                # x1/y1 fit into our current zone
                if x2 < x or x2 > x + w or y2 < y or y2 > y + h:
                    logger.debug(f"Barrier {id} spans multiple zones")
                elif x1 == x2 and (x1 != x and x1 != x + w):
                    logger.debug(f"Barrier {id} is not on vertical edge")
                elif y1 == y2 and (y1 != y and y1 != y + h):
                    logger.debug(f"Barrier {id} is not on horizontal edge")
                else:
                    self.current_barriers.append(Barrier(id=id, position=b["position"]))
                    break

                failed_barriers.append(id)
                break
            else:
                logger.debug(f"Barrier {id} does not fit into any zone")
                failed_barriers.append(id)
                continue

        response = Response(0, {})
        response.results["failed_barriers"] = dbus.Array(
            [dbus.UInt32(f) for f in failed_barriers],
            signature="u",
            variant_level=1,
        )

        logger.debug(f"SetPointerBarriers with response {response}")

        return response.response, response.results
    except Exception as e:
        logger.critical(e)
        return (2, {})


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="ua{sv}",
)
def Enable(self, session_handle, app_id, options):
    try:
        logger.debug(f"Enable({session_handle}, {options})")

        assert session_handle in self.sessions

        # for use in the signals
        activation_id = next(serials)
        barrier = self.current_barriers[0]
        pos = (barrier.position[0] + 10, barrier.position[1] + 20)

        if self.disable_delay > 0:

            def disable():
                logger.debug("emitting Disabled")
                self.EmitSignal("", "Disabled", "oa{sv}", [session_handle, {}])

            GLib.timeout_add(self.disable_delay, disable)

        if self.activated_delay > 0:

            def activated():
                logger.debug("emitting Activated")
                options = {
                    "activation_id": dbus.UInt32(activation_id, variant_level=1),
                    "barrier_id": dbus.UInt32(barrier.id, variant_level=1),
                    "cursor_position": dbus.Struct(
                        pos, signature="dd", variant_level=1
                    ),
                }
                self.EmitSignal("", "Activated", "oa{sv}", [session_handle, options])

            GLib.timeout_add(self.activated_delay, activated)

        if self.deactivated_delay > 0:

            def deactivated():
                logger.debug("emitting Deactivated")
                options = {
                    "activation_id": dbus.UInt32(activation_id, variant_level=1),
                    "cursor_position": dbus.Struct(
                        pos, signature="dd", variant_level=1
                    ),
                }
                self.EmitSignal("", "Deactivated", "oa{sv}", [session_handle, options])

            GLib.timeout_add(self.deactivated_delay, deactivated)

    except Exception as e:
        logger.critical(e)
        return (2, {})


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="ua{sv}",
)
def Disable(self, session_handle, app_id, options):
    try:
        logger.debug(f"Disable({session_handle}, {options})")

        assert session_handle in self.sessions
    except Exception as e:
        logger.critical(e)
        return (2, {})


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="ua{sv}",
)
def Release(self, session_handle, app_id, options):
    try:
        logger.debug(f"Release({session_handle}, {options})")

        assert session_handle in self.sessions
    except Exception as e:
        logger.critical(e)
        return (2, {})


@dbus.service.method(
    MAIN_IFACE,
    in_signature="osa{sv}",
    out_signature="h",
)
def ConnectToEIS(self, session_handle, app_id, options):
    try:
        logger.debug(f"ConnectToEIS({session_handle}, {options})")

        assert session_handle in self.sessions

        sockets = socket.socketpair()
        self.eis_socket = sockets[0]

        assert self.eis_socket.send(b"HELLO") == 5

        fd = sockets[1]

        logger.debug(f"ConnectToEis with fd {fd.fileno()}")

        return dbus.types.UnixFd(fd)
    except Exception as e:
        logger.critical(e)
        return -1
