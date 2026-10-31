# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import errno
import os
import stat
from collections.abc import Generator
from pathlib import Path
from typing import Any

import dbus
import pytest

import tests.xdp_doc_utils as xdp_doc
import tests.xdp_utils as xdp

type Directory = tuple[Path, Path]
type ReadDocument = tuple[str, Path, str]
type WriteDocument = tuple[str, Path, str, Path]


@pytest.fixture
def xdp_app_info() -> xdp.AppInfo:
    return xdp.AppInfoHost(app_id="")


@pytest.fixture
def documents_intf(xdg_document_portal: Any, dbus_con: dbus.Bus) -> dbus.Interface:
    return xdp.get_document_portal_iface(dbus_con)


@pytest.fixture
def mountpoint(documents_intf: dbus.Interface) -> Path:
    return xdp_doc.get_mountpoint(documents_intf)


class TestRegularDocPerms:
    def test_no_mkdir_in_doc_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-mkdir")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        with pytest.raises(PermissionError):
            os.mkdir(str(mountpoint / doc_id / "dir"))

    def test_no_symlink_in_doc_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-symlink")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        with pytest.raises(PermissionError):
            os.symlink("val", str(mountpoint / doc_id / "symlink"))

    def test_no_hardlink(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-hardlink")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name
        with pytest.raises(PermissionError):
            os.link(str(fuse_path), str(mountpoint / doc_id / "hardlink"))

    def test_no_rmdir_on_file(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("not-dir")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name
        with pytest.raises(NotADirectoryError):
            os.rmdir(str(fuse_path))

    def test_no_setxattr(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-xattr")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name
        with pytest.raises(PermissionError):
            os.setxattr(str(fuse_path), "user.attr", b"foo")

    def test_no_removexattr(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-rmxattr")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name
        with pytest.raises(PermissionError):
            os.removexattr(str(fuse_path), "user.attr")

    def test_no_rmdir_doc_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-rmdir-doc")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        with pytest.raises(PermissionError):
            os.rmdir(str(mountpoint / doc_id))

    def test_no_rename_doc_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-rename-doc")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        doc_path = str(mountpoint / doc_id)
        with pytest.raises(PermissionError):
            os.rename(doc_path, doc_path + "_renamed")

    def test_no_unlink_doc_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-unlink-doc")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        with pytest.raises(IsADirectoryError):
            os.unlink(str(mountpoint / doc_id))

    def test_no_cross_device_rename_out(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("no-xdev-rename")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        docpath = str(mountpoint / doc_id / file_path.name)
        with pytest.raises(OSError) as exc_info:
            os.rename(docpath, str(mountpoint / doc_id / ".." / file_path.name))
        assert exc_info.value.errno == errno.EXDEV


class TestReadOnlyAppPerms:
    @pytest.fixture
    def read_doc(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> ReadDocument:
        file_path = xdp_doc.make_file("readonly-data")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.read.Perms"
        documents_intf.GrantPermissions(doc_id, app_id, ["read"])
        app_path = mountpoint / "by-app" / app_id / doc_id
        return doc_id, app_path, file_path.name

    def test_can_read(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        fd = os.open(str(app_path / name), os.O_RDONLY)
        os.close(fd)

    def test_no_write(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.open(str(app_path / name), os.O_WRONLY)

    def test_no_rdwr(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.open(str(app_path / name), os.O_RDWR)

    def test_no_truncate(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.open(str(app_path / name), os.O_RDONLY | os.O_TRUNC)

    def test_no_rename(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.rename(str(app_path / name), str(app_path / "renamed"))

    def test_no_os_truncate(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.truncate(str(app_path / name), 1)

    def test_no_unlink(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.unlink(str(app_path / name))

    def test_no_utime(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        with pytest.raises(PermissionError):
            os.utime(str(app_path / name))

    def test_no_create_tmpfile(self, read_doc: ReadDocument) -> None:
        _, app_path, _ = read_doc
        for flags in [
            os.O_CREAT | os.O_RDONLY | os.O_TRUNC,
            os.O_CREAT | os.O_WRONLY,
            os.O_CREAT | os.O_RDWR,
        ]:
            with pytest.raises(PermissionError):
                os.open(str(app_path / "tmpfile"), flags, 0o600)

    def test_doc_dir_read_only_mode(self, read_doc: ReadDocument) -> None:
        _, app_path, _ = read_doc
        info = app_path.stat()
        assert info.st_mode == stat.S_IFDIR | 0o500

    def test_not_writable(self, read_doc: ReadDocument) -> None:
        _, app_path, _ = read_doc
        assert not os.access(str(app_path), os.W_OK)

    def test_file_not_writable(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        assert not os.access(str(app_path / name), os.W_OK)

    def test_stat_masks_write_bits(self, read_doc: ReadDocument) -> None:
        _, app_path, name = read_doc
        info = (app_path / name).stat()
        assert not (info.st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))


class TestNoPermApp:
    def test_doc_not_visible(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("invisible")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.NoPerm"
        app_path = mountpoint / "by-app" / app_id / doc_id
        with pytest.raises(FileNotFoundError):
            os.lstat(str(app_path))

    def test_regular_doc_not_visible_no_mkdir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        file_path = xdp_doc.make_file("invisible2")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.NoPerm2"
        app_path = mountpoint / "by-app" / app_id / doc_id
        with pytest.raises(PermissionError):
            os.mkdir(str(app_path))

    def test_dir_doc_not_visible_no_mkdir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        app_id = "com.test.NoPerm3"
        app_path = mountpoint / "by-app" / app_id / doc_id
        with pytest.raises(FileNotFoundError):
            os.lstat(str(app_path))
        with pytest.raises(PermissionError):
            os.mkdir(str(app_path))


class TestReadOnlyNamedNonExisting:
    def test_cannot_create(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        tmpdir = xdp_doc.make_dir()
        file_path = tmpdir / "no-file"
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        app_id = "com.test.read.Named"
        documents_intf.GrantPermissions(doc_id, app_id, ["read"])
        app_path = mountpoint / "by-app" / app_id / doc_id / "no-file"
        for flags in [
            os.O_CREAT | os.O_RDONLY | os.O_TRUNC,
            os.O_CREAT | os.O_WRONLY,
            os.O_CREAT | os.O_RDWR,
        ]:
            with pytest.raises(PermissionError):
                os.open(str(app_path), flags, 0o600)

    def test_appears_when_created_outside(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        tmpdir = xdp_doc.make_dir()
        file_path = tmpdir / "outside"
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        app_id = "com.test.read.NamedAppear"
        documents_intf.GrantPermissions(doc_id, app_id, ["read"])
        app_path = mountpoint / "by-app" / app_id / doc_id / "outside"

        assert not app_path.exists()
        file_path.write_text("from-outside")
        assert app_path.read_text() == "from-outside"

        with pytest.raises(PermissionError):
            app_path.unlink()
        file_path.unlink()
        assert not app_path.exists()


class TestWritableAppFileIO:
    @pytest.fixture
    def write_doc(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> WriteDocument:
        file_path = xdp_doc.make_file("write-data")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.write.IO"
        documents_intf.GrantPermissions(doc_id, app_id, ["read", "write"])
        app_path = mountpoint / "by-app" / app_id / doc_id
        return doc_id, app_path, file_path.name, mountpoint

    def test_no_cross_device_rename_out(self, write_doc: WriteDocument) -> None:
        _, app_path, name, _ = write_doc
        docpath = str(app_path / name)
        with pytest.raises(OSError) as exc_info:
            os.rename(docpath, str(app_path / ".." / name))
        assert exc_info.value.errno == errno.EXDEV

    def test_no_cross_device_rename_to_other_app_view(
        self, write_doc: WriteDocument, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        doc_id, app_path, name, _ = write_doc
        docpath = str(app_path / name)
        other_app = "com.test.write.Other"
        documents_intf.GrantPermissions(doc_id, other_app, ["read", "write"])
        other_path = mountpoint / "by-app" / other_app / doc_id / name
        with pytest.raises(OSError) as exc_info:
            os.rename(docpath, str(other_path))
        assert exc_info.value.errno == errno.EXDEV

    def test_writable_doc_dir_mode(self, write_doc: WriteDocument) -> None:
        _, app_path, _, _ = write_doc
        info = app_path.stat()
        assert info.st_mode == stat.S_IFDIR | 0o700

    def test_no_hardlink_on_doc_file(self, write_doc: WriteDocument) -> None:
        _, app_path, name, _ = write_doc
        docpath = str(app_path / name)
        with pytest.raises(PermissionError):
            os.link(docpath, str(app_path / "a-hardlink"))

    def test_no_setxattr_on_doc_file(
        self, write_doc: tuple[str, Path, str, Path]
    ) -> None:
        _, app_path, name, _ = write_doc
        docpath = str(app_path / name)
        with pytest.raises(PermissionError):
            os.setxattr(docpath, "user.attr", b"foo")
        with pytest.raises(PermissionError):
            os.removexattr(docpath, "user.attr")


class TestWritableNamedNonExisting:
    def test_can_create_and_delete(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> None:
        tmpdir = xdp_doc.make_dir()
        file_path = tmpdir / "to-create"
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        app_id = "com.test.write.NamedCreate"
        documents_intf.GrantPermissions(doc_id, app_id, ["read", "write"])
        app_fuse_path = mountpoint / "by-app" / app_id / doc_id / "to-create"

        assert not app_fuse_path.exists()
        app_fuse_path.write_text("some-data")
        assert app_fuse_path.read_text() == "some-data"
        app_fuse_path.unlink()
        assert not app_fuse_path.exists()


class TestDirDocPerms:
    @pytest.fixture
    def dir_doc(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> tuple[str, Path, Path]:
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        return doc_id, dir_path, mountpoint

    def test_toplevel_doc_dir_no_create(self, dir_doc: tuple[str, Path, Path]) -> None:
        doc_id, _, mountpoint = dir_doc
        vpath = mountpoint / doc_id
        with pytest.raises(PermissionError):
            os.mkdir(str(vpath / "a_dir"))
        with pytest.raises(PermissionError):
            os.open(str(vpath / "a_file"), os.O_CREAT | os.O_RDWR, 0o600)

    def test_toplevel_doc_dir_no_rename(self, dir_doc: tuple[str, Path, Path]) -> None:
        doc_id, dir_path, mountpoint = dir_doc
        docpath = mountpoint / doc_id / dir_path.name
        vpath = mountpoint / doc_id
        with pytest.raises(PermissionError):
            os.rename(str(docpath), str(vpath / "foo"))

    def test_toplevel_doc_dir_no_rmdir(self, dir_doc: tuple[str, Path, Path]) -> None:
        doc_id, dir_path, mountpoint = dir_doc
        docpath = mountpoint / doc_id / dir_path.name
        with pytest.raises(PermissionError):
            os.rmdir(str(docpath))

    def test_toplevel_lists_real_dir(self, dir_doc: tuple[str, Path, Path]) -> None:
        doc_id, dir_path, mountpoint = dir_doc
        entries = os.listdir(mountpoint / doc_id)
        assert entries == [dir_path.name]

    def test_dir_contents_visible(self, dir_doc: tuple[str, Path, Path]) -> None:
        doc_id, dir_path, mountpoint = dir_doc
        docpath = mountpoint / doc_id / dir_path.name

        subdir = dir_path / "sub"
        subdir.mkdir()
        (dir_path / "realfile").write_text("real1")
        (subdir / "hardlink").write_text("real1")
        os.symlink("realfile", str(dir_path / "symlink"))
        os.symlink("the-void", str(dir_path / "broken-symlink"))

        assert (docpath / "realfile").read_text() == "real1"
        assert (docpath / "sub").is_dir()
        assert (docpath / "sub" / "hardlink").read_text() == "real1"
        assert os.readlink(str(docpath / "symlink")) == "realfile"
        assert os.readlink(str(docpath / "broken-symlink")) == "the-void"

        entries = sorted(os.listdir(docpath))
        assert "realfile" in entries
        assert "sub" in entries
        assert "symlink" in entries
        assert "broken-symlink" in entries

        os.unlink(str(dir_path / "realfile"))
        os.unlink(str(subdir / "hardlink"))
        os.unlink(str(dir_path / "symlink"))
        os.unlink(str(dir_path / "broken-symlink"))
        subdir.rmdir()

    def test_hardlinks_share_inode(self, dir_doc: tuple[str, Path, Path]) -> None:
        doc_id, dir_path, mountpoint = dir_doc
        docpath = mountpoint / doc_id / dir_path.name

        subdir = dir_path / "sub"
        subdir.mkdir()
        (dir_path / "realfile").write_text("real1")
        os.link(str(dir_path / "realfile"), str(subdir / "hardlink"))

        assert (docpath / "realfile").stat().st_ino == (
            docpath / "sub" / "hardlink"
        ).stat().st_ino

        os.unlink(str(dir_path / "realfile"))
        os.unlink(str(subdir / "hardlink"))
        subdir.rmdir()


class TestDirDocWritable:
    @pytest.fixture
    def writable_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> Directory:
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        app_id = "com.test.write.Dir"
        documents_intf.GrantPermissions(doc_id, app_id, ["read", "write"])
        docpath = mountpoint / "by-app" / app_id / doc_id / dir_path.name
        return docpath, dir_path

    def test_no_cross_device_rename(
        self, writable_dir: Directory, mountpoint: Path
    ) -> None:
        docpath, _ = writable_dir
        with pytest.raises(OSError) as exc_info:
            os.rename(str(docpath), str(mountpoint))
        assert exc_info.value.errno == errno.EXDEV

    def test_chmod_readonly_file(
        self, writable_dir: Directory, mountpoint: Path
    ) -> None:
        docpath, dir_path = writable_dir
        (dir_path / "readonly").write_text("readonly")
        os.chmod(str(dir_path / "readonly"), 0o500)

        if os.environ.get("TEST_IN_ROOTED_CI"):
            with pytest.raises(PermissionError):
                os.open(str(docpath / "readonly"), os.O_RDWR)
            os.chmod(str(docpath / "readonly"), 0o700)
            fd = os.open(str(docpath / "readonly"), os.O_RDWR)
            os.close(fd)

        os.chmod(str(dir_path / "readonly"), 0o700)
        os.unlink(str(dir_path / "readonly"))


class TestDirDocReadOnly:
    @pytest.fixture
    def readonly_dir(
        self, documents_intf: dbus.Interface, mountpoint: Path
    ) -> Generator[Directory]:
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        app_id = "com.test.read.Dir"
        documents_intf.GrantPermissions(doc_id, app_id, ["read"])

        subdir = dir_path / "sub"
        subdir.mkdir()
        (dir_path / "realfile").write_text("real1")
        (dir_path / "readonly").write_text("readonly")
        os.chmod(str(dir_path / "readonly"), 0o500)

        docpath = mountpoint / "by-app" / app_id / doc_id / dir_path.name
        yield docpath, dir_path

        for f in ["realfile", "readonly"]:
            p = dir_path / f
            if p.exists():
                os.chmod(str(p), 0o700)
                p.unlink()
        if subdir.exists():
            subdir.rmdir()

    def test_no_create_file(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        for flags in [
            os.O_CREAT | os.O_RDONLY | os.O_TRUNC,
            os.O_CREAT | os.O_WRONLY,
            os.O_CREAT | os.O_RDWR,
        ]:
            with pytest.raises(PermissionError):
                os.open(str(docpath / "newfile"), flags, 0o600)

    def test_no_write_existing(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.open(str(docpath / "realfile"), os.O_RDWR)

    def test_no_write_readonly_file(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.open(str(docpath / "readonly"), os.O_RDWR)

    def test_no_truncate(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.truncate(str(docpath / "realfile"), 0)

    def test_no_link(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.link(str(docpath / "realfile"), str(docpath / "foo"))

    def test_no_symlink(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.symlink("foo", str(docpath / "new-symlink"))

    def test_no_rename(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.rename(str(docpath / "realfile"), str(docpath / "foo"))

    def test_no_unlink(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.unlink(str(docpath / "realfile"))

    def test_no_chmod(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.chmod(str(docpath / "realfile"), 0o700)

    def test_no_rmdir_subdir(self, readonly_dir: Directory) -> None:
        docpath, _ = readonly_dir
        with pytest.raises(PermissionError):
            os.rmdir(str(docpath / "sub"))


try:
    xdp.ensure_fuse_supported()
except xdp.FuseNotSupportedException as e:
    pytest.skip(f"No fuse support: {e}", allow_module_level=True)
