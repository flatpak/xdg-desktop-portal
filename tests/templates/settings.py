# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from dataclasses import dataclass
from typing import cast

import dbus
import dbus.service
import dbusmock
from gi.repository import GLib

from tests.templates.xdp_utils import init_logger

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Settings"
VERSION = 2


logger = init_logger(__name__)


type ReadAllOutput = dict[str, dict[str, GLib.Variant]] | GLib.Variant


@dataclass
class SettingsParameters:
    settings: dict[str, GLib.Variant]


def load(mock: dbusmock.DBusMockObject, parameters: GLib.Variant | None = None) -> None:
    parameters = parameters or {}
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "settings_params")
    mock.settings_params = SettingsParameters(
        settings=parameters.get("settings", {}),
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
    in_signature="as",
    out_signature="a{sa{sv}}",
)
def ReadAll(self, namespaces: list[str]) -> ReadAllOutput:  # type: ignore [no-untyped-def]
    logger.debug(f"ReadAll({namespaces})")
    settings = self.settings_params.settings

    if len(namespaces) == 0 or (len(namespaces) == 1 and namespaces[0] == ""):
        return settings

    def find_matching(namespace: str) -> dict[str, str]:
        if len(namespace) >= 3 and namespace[-2:] == ".*":
            ns_prefix = namespace[:-2]
            matches = {}
            for ns in settings:
                if ns.startswith(ns_prefix):
                    matches[ns] = settings[ns]
            return matches

        if namespace in settings:
            return {namespace: settings[namespace]}

        return {}

    result = dbus.Dictionary({}, signature="sa{sv}")
    for ns in namespaces:
        result |= find_matching(ns)

    return cast("ReadAllOutput", result)


@dbus.service.method(  # type: ignore [untyped-decorator]
    MAIN_IFACE,
    in_signature="ss",
    out_signature="v",
)
def Read(self, namespace: str, key: str) -> GLib.Variant:  # type: ignore [no-untyped-def]
    logger.debug(f"Read({namespace}, {key})")
    settings = self.settings_params.settings

    return settings[namespace][key]


@dbus.service.method(  # type: ignore [untyped-decorator]
    dbusmock.MOCK_IFACE,
    in_signature="ssv",
    out_signature="",
)
def SetSetting(self, namespace: str, key: str, value: GLib.Variant) -> None:  # type: ignore [no-untyped-def]
    logger.debug(f"SetSetting({namespace}, {key}, {value})")
    settings = self.settings_params.settings

    settings.setdefault(namespace, {})[key] = value

    self.EmitSignal(
        MAIN_IFACE,
        "SettingChanged",
        "ssv",
        [namespace, key, value],
    )
