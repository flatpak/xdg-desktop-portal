# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates import init_logger

import dbus.service
from dbusmock import MOCK_IFACE


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Notification"
VERSION = 2


logger = init_logger(__name__)


def load(mock, parameters={}):
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


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ssa{sv}",
    out_signature="",
)
def AddNotification(self, app_id, id, notification):
    logger.debug(f"AddNotification({app_id}, {id}, {notification})")

    self.notifications.setdefault(app_id, {})[id] = notification


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ss",
    out_signature="",
)
def RemoveNotification(self, app_id, id):
    logger.debug(f"AddNotification({app_id}, {id})")

    del self.notifications[app_id][id]


@dbus.service.method(
    MOCK_IFACE,
    in_signature="sssav",
    out_signature="",
)
def EmitActionInvoked(self, app_id, id, action, parameter):
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
