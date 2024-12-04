# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import init_template_logger

import dbus.service
from dbusmock import MOCK_IFACE


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Settings"
VERSION = 2


logger = init_template_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.settings = parameters.get("settings", {})
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
    in_signature="as",
    out_signature="a{sa{sv}}",
)
def ReadAll(self, namespaces):
    logger.debug(f"ReadAll({namespaces})")

    if len(namespaces) == 0 or (len(namespaces) == 1 and namespaces[0] == ""):
        return self.settings

    def find_matching(namespace):
        if len(namespace) >= 3 and namespace[-2:] == ".*":
            ns_prefix = namespace[:-2]
            matches = {}
            for ns in self.settings:
                if ns.startswith(ns_prefix):
                    matches[ns] = self.settings[ns]
            return matches

        if namespace in self.settings:
            return {namespace: self.settings[namespace]}

        return {}

    result = dbus.Dictionary({}, signature="sa{sv}")
    for ns in namespaces:
        result |= find_matching(ns)

    return result


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ss",
    out_signature="v",
)
def Read(self, namespace, key):
    logger.debug(f"Read({namespace}, {key})")

    return self.settings[namespace][key]


@dbus.service.method(
    MOCK_IFACE,
    in_signature="ssv",
    out_signature="",
)
def SetSetting(self, namespace, key, value):
    self.settings.setdefault(namespace, {})[key] = value

    self.EmitSignal(
        MAIN_IFACE,
        "SettingChanged",
        "ssv",
        [namespace, key, value],
    )
