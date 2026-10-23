# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import os
import stat

import dbus
import pytest

import tests.xdp_doc_utils as xdp_doc
import tests.xdp_utils as xdp


@pytest.fixture
def xdp_app_info() -> xdp.AppInfo:
    return xdp.AppInfoHost(app_id="")


@pytest.fixture
def documents_intf(xdg_document_portal, dbus_con):
    return xdp.get_document_portal_iface(dbus_con)


@pytest.fixture
def mountpoint(documents_intf):
    return xdp_doc.get_mountpoint(documents_intf)


class TestFuseLayout:
    def test_root_lists_docs_and_by_app(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file()
        doc_id = xdp_doc.export_file(documents_intf, file_path)

        entries = os.listdir(mountpoint)
        assert "by-app" in entries
        assert doc_id in entries

    def test_root_stat(self, documents_intf, mountpoint):
        info = mountpoint.stat()
        assert info.st_uid == os.getuid()
        assert info.st_gid == os.getgid()
        assert info.st_mode == stat.S_IFDIR | 0o500

    def test_root_not_writable(self, documents_intf, mountpoint):
        assert not os.access(mountpoint, os.W_OK)

    def test_root_no_create_file(self, documents_intf, mountpoint):
        with pytest.raises(PermissionError):
            os.open(str(mountpoint / "a-file"), os.O_RDWR | os.O_CREAT)

    def test_root_no_mkdir(self, documents_intf, mountpoint):
        with pytest.raises(PermissionError):
            os.mkdir(str(mountpoint / "a_dir"))

    def test_root_no_rename_by_app(self, documents_intf, mountpoint):
        with pytest.raises(PermissionError):
            os.rename(str(mountpoint / "by-app"), str(mountpoint / "by-app2"))

    def test_root_no_rmdir_by_app(self, documents_intf, mountpoint):
        with pytest.raises(PermissionError):
            os.rmdir(str(mountpoint / "by-app"))

    def test_root_nonexistent_file(self, documents_intf, mountpoint):
        with pytest.raises(FileNotFoundError):
            os.lstat(str(mountpoint / "not-existing-file"))

    def test_by_app_stat(self, documents_intf, mountpoint):
        info = (mountpoint / "by-app").stat()
        assert info.st_uid == os.getuid()
        assert info.st_gid == os.getgid()
        assert info.st_mode == stat.S_IFDIR | 0o500

    def test_by_app_no_create(self, documents_intf, mountpoint):
        by_app = mountpoint / "by-app"
        with pytest.raises(PermissionError):
            os.mkdir(str(by_app / "a_dir"))
        with pytest.raises(PermissionError):
            os.open(str(by_app / "a-file"), os.O_RDWR | os.O_CREAT)

    def test_app_dir_created_on_lookup(self, documents_intf, mountpoint):
        app_id = "org.test.LookupApp"
        app_path = mountpoint / "by-app" / app_id
        info = app_path.stat()
        assert info.st_uid == os.getuid()
        assert info.st_gid == os.getgid()
        assert info.st_mode == stat.S_IFDIR | 0o500

    def test_invalid_app_id_not_found(self, documents_intf, mountpoint):
        with pytest.raises(FileNotFoundError):
            os.lstat(str(mountpoint / "by-app" / "not-an-app-id"))

    def test_app_dir_shows_granted_docs(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("data")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.LayoutApp"
        documents_intf.GrantPermissions(doc_id, app_id, ["read"])

        entries = os.listdir(mountpoint / "by-app" / app_id)
        assert doc_id in entries


class TestDocExport:
    def test_add_and_lookup(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("hello")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        assert (
            documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(file_path)) == doc_id
        )

    def test_add_reuse_existing(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("reuse")
        doc_id1 = xdp_doc.export_file(documents_intf, file_path)
        doc_id2 = xdp_doc.export_file(documents_intf, file_path)
        assert doc_id1 == doc_id2

    def test_add_unique(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("unique")
        doc_id1 = xdp_doc.export_file(documents_intf, file_path)
        doc_id2 = xdp_doc.export_file(documents_intf, file_path, unique=True)
        assert doc_id1 != doc_id2

    def test_lookup_on_fuse_path(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("fuse-lookup")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name
        assert (
            documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(fuse_path)) == doc_id
        )

    def test_add_named_existing(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("named-exist")
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        doc_path = mountpoint / doc_id / file_path.name
        assert doc_path.read_text() == "named-exist"

    def test_add_named_nonexisting(self, documents_intf, mountpoint):
        tmpdir = xdp_doc.make_dir()
        file_path = tmpdir / "not-yet"
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        doc_path = mountpoint / doc_id
        assert doc_path.exists()
        assert not (doc_path / file_path.name).exists()

    def test_add_named_reuse(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("named-reuse")
        doc_id1 = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        doc_id2 = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        assert doc_id1 == doc_id2

    def test_add_named_unique(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("named-unique")
        doc_id1 = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        doc_id2 = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name, unique=True
        )
        assert doc_id1 != doc_id2

    def test_add_dir(self, documents_intf, mountpoint):
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        doc_path = mountpoint / doc_id / dir_path.name
        assert doc_path.is_dir()

    def test_add_dir_lookup(self, documents_intf, mountpoint):
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        assert (
            documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(dir_path)) == doc_id
        )

    def test_add_dir_lookup_on_fuse(self, documents_intf, mountpoint):
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        fuse_dir = mountpoint / doc_id / dir_path.name
        assert (
            documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(fuse_dir)) == doc_id
        )

    def test_tmpfile_cannot_be_exported(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("no-reexport")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        doc_dir = mountpoint / doc_id
        tmp = doc_dir / "tmpfile"
        tmp.write_text("tempdata")

        with pytest.raises(dbus.exceptions.DBusException):
            xdp_doc.export_file(documents_intf, tmp)

        assert documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(tmp)) == ""
        tmp.unlink()

    def test_dir_subfile_not_in_lookup(self, documents_intf, mountpoint):
        dir_path = xdp_doc.make_dir()
        doc_id = xdp_doc.export_dir(documents_intf, dir_path)
        doc_path = mountpoint / doc_id / dir_path.name

        subfile = doc_path / "sub"
        subfile.write_text("sub")

        assert documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(subfile)) == ""
        assert (
            documents_intf.Lookup(xdp_doc.path_to_null_term_bytes(dir_path / "sub"))
            == ""
        )

        subfile.unlink()

    def test_dir_subfile_can_be_reexported(self, documents_intf, mountpoint):
        dir_path = xdp_doc.make_dir()
        xdp_doc.export_dir(documents_intf, dir_path)
        subfile = dir_path / "sub"
        subfile.write_text("sub")

        sub_doc_id = xdp_doc.export_file(documents_intf, subfile)
        sub_doc_path = mountpoint / sub_doc_id
        assert (sub_doc_path / "sub").read_text() == "sub"

        documents_intf.Delete(sub_doc_id)
        subfile.unlink()

    def test_dir_subdir_can_be_reexported(self, documents_intf, mountpoint):
        dir_path = xdp_doc.make_dir()
        xdp_doc.export_dir(documents_intf, dir_path)
        subdir = dir_path / "subdir"
        subdir.mkdir()
        (subdir / "subfile").write_text("subfile")

        sub_doc_id = xdp_doc.export_dir(documents_intf, subdir)
        sub_doc_path = mountpoint / sub_doc_id / "subdir"
        assert (sub_doc_path / "subfile").read_text() == "subfile"

        documents_intf.Delete(sub_doc_id)
        (subdir / "subfile").unlink()
        subdir.rmdir()


class TestDocContent:
    def test_file_stat_matches_real(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("stat-check")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name

        fuse_st = fuse_path.stat()
        real_st = file_path.stat()
        mode_mask = ~(stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX)
        assert fuse_st.st_mode == (real_st.st_mode & mode_mask)
        assert fuse_st.st_size == real_st.st_size
        assert fuse_st.st_uid == real_st.st_uid
        assert fuse_st.st_gid == real_st.st_gid
        assert fuse_st.st_nlink == real_st.st_nlink
        assert fuse_st.st_atime == real_st.st_atime
        assert fuse_st.st_mtime == real_st.st_mtime
        assert fuse_st.st_ctime == real_st.st_ctime

    def test_named_nonexisting_file_not_accessible(self, documents_intf, mountpoint):
        tmpdir = xdp_doc.make_dir()
        file_path = tmpdir / "ghost"
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        fuse_path = mountpoint / doc_id / "ghost"

        with pytest.raises(FileNotFoundError):
            fuse_path.stat()
        with pytest.raises(FileNotFoundError):
            os.open(str(fuse_path), os.O_RDONLY)

    def test_named_file_appears_when_created_externally(
        self, documents_intf, mountpoint
    ):
        tmpdir = xdp_doc.make_dir()
        file_path = tmpdir / "appear"
        doc_id = xdp_doc.export_file_named(
            documents_intf, file_path.parent, file_path.name
        )
        fuse_path = mountpoint / doc_id / "appear"

        assert not fuse_path.exists()
        file_path.write_text("from-outside")
        assert fuse_path.read_text() == "from-outside"

        fuse_path.unlink()

    def test_doc_dir_has_no_xdp_tempfiles(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("clean")
        xdp_doc.export_file(documents_intf, file_path)
        for entry in os.listdir(file_path.parent):
            assert not entry.startswith(".xdp")

    def test_doc_dir_lists_only_doc_file(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("only-me")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        entries = os.listdir(mountpoint / doc_id)
        assert entries == [file_path.name]

    def test_doc_dir_stat(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("dir-stat")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        doc_dir = mountpoint / doc_id
        info = doc_dir.stat()
        assert info.st_uid == os.getuid()
        assert info.st_gid == os.getgid()
        assert info.st_mode == stat.S_IFDIR | 0o700


try:
    xdp.ensure_fuse_supported()
except xdp.FuseNotSupportedException as e:
    pytest.skip(f"No fuse support: {e}", allow_module_level=True)
