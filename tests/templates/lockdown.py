# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import init_logger

import dbus.service


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Lockdown"


logger = init_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "disable-printing": dbus.Boolean(
                    parameters.get("disable-printing", False)
                ),
                "disable-save-to-disk": dbus.Boolean(
                    parameters.get("disable-save-to-disk", False)
                ),
                "disable-application-handlers": dbus.Boolean(
                    parameters.get("disable-application-handlers", False)
                ),
                "disable-location": dbus.Boolean(
                    parameters.get("disable-location", False)
                ),
                "disable-camera": dbus.Boolean(parameters.get("disable-camera", False)),
                "disable-microphone": dbus.Boolean(
                    parameters.get("disable-microphone", False)
                ),
                "disable-sound-output": dbus.Boolean(
                    parameters.get("disable-sound-output", False)
                ),
            }
        ),
    )
