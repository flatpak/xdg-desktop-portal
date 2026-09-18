# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

from typing import Any

import dbus

import tests.xdp_utils as xdp


class TestMemoryMonitor:
    def test_version(self, portals: Any, dbus_con: dbus.Bus) -> None:
        xdp.check_version(dbus_con, "MemoryMonitor", 1)
