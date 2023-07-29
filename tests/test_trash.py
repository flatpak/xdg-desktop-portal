# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from tests import PortalTest
from pathlib import Path

import os
import tempfile


class TestTrash(PortalTest):
    def test_version(self):
        self.check_version(1)

    def test_trash_file_fails(self):
        self.start_xdp()

        trash_intf = self.get_dbus_interface()
        with open("/proc/cmdline") as fd:
            result = trash_intf.TrashFile(fd.fileno())

        assert result == 0

    def test_trash_file(self):
        self.start_xdp()

        trash_intf = self.get_dbus_interface()

        fd, name = tempfile.mkstemp(prefix="trash_portal_test_", dir=Path.home())
        result = trash_intf.TrashFile(fd)
        if result != 1:
            os.unlink(name)
        assert result == 1
        assert not Path(name).exists()
