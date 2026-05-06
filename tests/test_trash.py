# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import os
import pytest
import tempfile
from pathlib import Path
from gi.repository import GLib


class TestTrash:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Trash", 1)

    def test_trash_file_fails(self, portals, dbus_con):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")
        try:
            with open("/proc/cmdline") as fd:
                result = trash_intf.TrashFile(fd.fileno())
        except PermissionError as e:
            pytest.skip(f"Couldn't open file /proc/cmdline: {e}")

        assert result == 0

    def test_trash_file(self, portals, dbus_con):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")

        fd, name = tempfile.mkstemp(prefix="trash_portal_mock_", dir=Path.home())
        result = trash_intf.TrashFile(fd)

        assert result == 1
        assert not Path(name).exists()

        info_dir = Path(os.environ["XDG_DATA_HOME"]) / "Trash/info"
        assert info_dir.exists()

        files_dir = Path(os.environ["XDG_DATA_HOME"]) / "Trash/files"
        assert files_dir.exists()

        trashed_files = info_dir.iterdir()
        trashed_file = next(trashed_files)

        keyfile = GLib.KeyFile.new()
        content = trashed_file.read_text()
        assert keyfile.load_from_data(
            content,
            len(content),
            GLib.KeyFileFlags.NONE,
        )
        assert keyfile.get_string("Trash Info", "Path") == name
        assert (files_dir / trashed_file.stem).exists()

        with pytest.raises(StopIteration):
            next(trashed_files)

    @pytest.mark.skip(reason="Portal requires write perm, so dirs are not supported")
    def test_trash_folder(self, portals, dbus_con):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")

        folder = Path(os.environ["HOME"]) / "folder-to-trash"
        file_in_folder = folder / "foo" / "bar" / "file"
        file_in_folder.parent.mkdir(parents=True)
        file_in_folder.write_text("foobar")

        fd = os.open(folder, os.O_RDONLY | os.O_CLOEXEC)
        try:
            result = trash_intf.TrashFile(fd)
        finally:
            os.close(fd)

        assert result == 1
        assert not Path(folder).exists()

        info_dir = Path(os.environ["XDG_DATA_HOME"]) / "Trash/info"
        assert info_dir.exists()

        files_dir = Path(os.environ["XDG_DATA_HOME"]) / "Trash/files"
        assert files_dir.exists()

        trashed_files = info_dir.iterdir()
        trashed_file = next(trashed_files)

        keyfile = GLib.KeyFile.new()
        content = trashed_file.read_text()
        assert keyfile.load_from_data(
            content,
            len(content),
            GLib.KeyFileFlags.NONE,
        )
        assert keyfile.get_string("Trash Info", "Path") == folder.as_posix()

        trashed_folder = files_dir / trashed_file.stem
        assert trashed_folder.exists()
        trashed_file = trashed_folder / "foo" / "bar" / "file"
        assert trashed_file.exists()

        with pytest.raises(StopIteration):
            next(trashed_files)
