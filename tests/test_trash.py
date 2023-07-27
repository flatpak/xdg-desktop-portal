# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from pathlib import Path

import os
import pytest
import tempfile


@pytest.fixture
def portal_name():
    return "Trash"


@pytest.fixture
def portal_has_impl():
    return False


class TestTrash:
    def test_version(self, portal_mock):
        portal_mock.check_version(1)

    def test_trash_file_fails(self, portal_mock):
        trash_intf = portal_mock.get_dbus_interface()
        with open("/proc/cmdline") as fd:
            result = trash_intf.TrashFile(fd.fileno())

        assert result == 0

    def test_trash_file(self, portal_mock):
        trash_intf = portal_mock.get_dbus_interface()

        fd, name = tempfile.mkstemp(prefix="trash_portal_mock_", dir=Path.home())
        result = trash_intf.TrashFile(fd)
        if result != 1:
            os.unlink(name)
        assert result == 1
        assert not Path(name).exists()
