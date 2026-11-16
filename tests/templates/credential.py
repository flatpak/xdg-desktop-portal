# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import dbus.service

from tests.templates.xdp_utils import init_logger

BUS_NAME = "xyz.iinuwa.credentialsd.Credentials"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.handler.portal.experimental.Credential"


logger = init_logger(__name__)


def load(mock, parameters=None):
    logger.debug(f"Loading parameters: {parameters}")


@dbus.service.method(
    MAIN_IFACE,
    in_signature="sssa{sv}s",
    out_signature="ua{sv}",
)
def CreateCredential(self, parent_window, origin, cred_type, options, app_id):
    logger.debug(
        f"CreateCredential({parent_window}, {origin}, {cred_type}, {options}, {app_id})"
    )
    return (dbus.UInt32(0), dbus.Dictionary({}, signature="sv"))


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ssa{sv}s",
    out_signature="ua{sv}",
)
def GetCredential(self, parent_window, origin, options, app_id):
    logger.debug(f"GetCredential({parent_window}, {origin}, {options}, {app_id})")
    return (dbus.UInt32(0), dbus.Dictionary({}, signature="sv"))
