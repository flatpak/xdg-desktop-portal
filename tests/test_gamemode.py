# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © Para-Real Ltd. (https://parare.al)
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

from typing import Any

import dbus

import tests.xdp_utils as xdp


class TestGameMode:
    def test_version(self, portals: Any, dbus_con: dbus.Bus) -> None:
        xdp.check_version(dbus_con, "GameMode", 4)
