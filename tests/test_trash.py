# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import os
import tempfile
from pathlib import Path


class TestTrash:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Trash", 1)

    def test_trash_file_fails(self, portals, dbus_con):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")
        with open("/proc/cmdline") as fd:
            result = trash_intf.TrashFile(fd.fileno())

        assert result == 0

    def test_trash_file(self, portals, dbus_con):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")

        fd, name = tempfile.mkstemp(prefix="trash_portal_mock_", dir=Path.home())
        result = trash_intf.TrashFile(fd)
        if result != 1:
            os.unlink(name)
        assert result == 1
        assert not Path(name).exists()
