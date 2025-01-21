# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import pytest
import dbus
from pathlib import Path
import os
from gi.repository import GLib


def path_from_null_term_bytes(bytes):
    path_bytes, rest = bytes.split(b"\x00")
    assert rest == b""
    return Path(os.fsdecode(path_bytes))


def get_mountpoint(documents_intf):
    mountpoint = documents_intf.GetMountPoint(byte_arrays=True)
    mountpoint = path_from_null_term_bytes(mountpoint)
    assert mountpoint.exists()
    return mountpoint


def write_bytes_atomic(file_path, bytes):
    GLib.file_set_contents(file_path.absolute().as_posix(), bytes)


def write_bytes_trunc(file_path, bytes):
    try:
        fd = os.open(
            file_path.absolute().as_posix(), os.O_RDWR | os.O_TRUNC | os.O_CREAT
        )
        os.write(fd, bytes)
    finally:
        os.close(fd)


def export_file(documents_intf, file_path, unique=False):
    assert file_path.exists()

    with open(file_path.absolute().as_posix(), "r") as file:
        doc_id = documents_intf.Add(file.fileno(), not unique, False)
        assert doc_id

    return doc_id


def export_file_named(documents_intf, folder_path, name, unique=False):
    assert folder_path.exists()

    # bytestring convention is zero terminated
    name_nt = os.fsencode(name) + b"\x00"

    try:
        fd = os.open(folder_path.absolute().as_posix(), os.O_PATH | os.O_CLOEXEC)
        doc_id = documents_intf.AddNamed(fd, name_nt, not unique, False)
        assert doc_id
    finally:
        os.close(fd)

    return doc_id


def export_files(documents_intf, file_paths, perms, flags=0, app_id=""):
    fds = []
    try:
        for file_path in file_paths:
            fds.append(
                os.open(file_path.absolute().as_posix(), os.O_PATH | os.O_CLOEXEC)
            )

        result = documents_intf.AddFull(
            fds,
            flags,
            app_id,
            perms,
            byte_arrays=True,
        )
    finally:
        for fd in fds:
            os.close(fd)

    assert result
    return result


class TestDocuments:
    def test_version(self, xdg_document_portal, dbus_con):
        documents = dbus_con.get_object(
            "org.freedesktop.portal.Documents",
            "/org/freedesktop/portal/documents",
        )

        properties_intf = dbus.Interface(
            documents,
            "org.freedesktop.DBus.Properties",
        )
        portal_version = properties_intf.Get(
            "org.freedesktop.portal.Documents",
            "version",
        )
        assert int(portal_version) == 5

    def test_mount_point(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        get_mountpoint(documents_intf)

    def test_create_doc(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = get_mountpoint(documents_intf)

        content = b"content"
        file_name = "a-file"

        file_path = Path(os.environ["TMPDIR"]) / file_name
        write_bytes_atomic(file_path, content)
        doc_id = export_file(documents_intf, file_path)

        doc_path = mountpoint / doc_id
        doc_app1_path = mountpoint / "by-app" / "com.test.App1" / doc_id
        doc_app2_path = mountpoint / "by-app" / "com.test.App2" / doc_id

        # Make sure it got exported
        assert (doc_path / file_name).read_bytes() == content

        assert not (doc_path / "another-file").exists()
        assert not (mountpoint / "anotherid" / file_name).exists()

        # Make sure it is not viewable by apps
        assert not doc_app1_path.exists()
        assert not doc_app2_path.exists()

        # Create a tmp file in same dir, ensure it works and can't be seen by other apps
        write_bytes_atomic(doc_path / "tmp1", b"tmpdata1")
        assert (doc_path / "tmp1").read_bytes() == b"tmpdata1"
        assert not (doc_app1_path / "tmp1").exists()

        # Ensure App 1 and only it can see the document and tmpfile
        documents_intf.GrantPermissions(doc_id, "com.test.App1", ["read"])
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Make sure App 1 can't create a tmpfile
        assert not (doc_app1_path / "tmp2").exists()
        with pytest.raises(PermissionError):
            (doc_app1_path / "tmp2").write_bytes(b"tmpdata1")
        assert not (doc_app1_path / "tmp2").exists()
        assert not (doc_path / "tmp2").exists()

        # Update the document contents, ensure this is propagated
        content = b"content2"
        write_bytes_atomic(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert file_path.read_bytes() == content
        assert not (doc_app2_path / file_name).exists()
        assert not (doc_app2_path / "tmp1").exists()

        # Update the document contents outside fuse fd, ensure this is propagated
        content = b"content3"
        write_bytes_atomic(file_path, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert file_path.read_bytes() == content
        assert not (doc_app2_path / file_name).exists()
        assert not (doc_app2_path / "tmp1").exists()

        # Try to update the doc from an app that can't write to it
        with pytest.raises(PermissionError):
            (doc_app1_path / file_name).write_bytes(b"content4")

        # Update the doc from an app with write access
        documents_intf.GrantPermissions(doc_id, "com.test.App1", ["write"])
        content = b"content5"
        write_bytes_atomic(doc_app1_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert file_path.read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Try to create a tmp file for an app
        assert not (doc_app1_path / "tmp3").exists()
        write_bytes_atomic(doc_app1_path / "tmp3", b"tmpdata2")
        (doc_app1_path / "tmp3").read_bytes() == b"tmpdata2"
        assert not (doc_path / "tmp3").exists()

        # Re-Create a file from a fuse document file, in various ways
        doc_id2 = export_file(documents_intf, (doc_path / file_name))
        assert doc_id2 == doc_id
        doc_id3 = export_file(documents_intf, (doc_app1_path / file_name))
        assert doc_id3 == doc_id
        doc_id4 = export_file(documents_intf, file_path)
        assert doc_id4 == doc_id

        # Ensure we can make a unique document
        doc_id5 = export_file(documents_intf, file_path, unique=True)
        assert doc_id5 != doc_id

    def test_recursive_doc(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = get_mountpoint(documents_intf)

        content = b"content"
        file_name = "recursive-file"

        file_path = Path(os.environ["TMPDIR"]) / file_name
        write_bytes_atomic(file_path, content)
        doc_id = export_file(documents_intf, file_path)

        doc_path = mountpoint / doc_id
        doc_app1_path = mountpoint / "by-app" / "com.test.App1" / doc_id

        assert (doc_path / file_name).read_bytes() == content

        doc_id2 = export_file(documents_intf, doc_path / file_name)
        assert doc_id2 == doc_id

        documents_intf.GrantPermissions(doc_id, "com.test.App1", ["read"])

        doc_id3 = export_file(documents_intf, doc_app1_path / file_name)
        assert doc_id3 == doc_id

    def test_create_docs(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = get_mountpoint(documents_intf)

        files = {
            "doc1": b"doc1-content",
            "doc2": b"doc2-content",
        }

        file_paths = []
        for file_name, file_content in files.items():
            file_path = Path(os.environ["TMPDIR"]) / file_name
            write_bytes_atomic(file_path, file_content)
            file_paths.append(file_path)

        doc_ids, extra = export_files(
            documents_intf, file_paths, ["read"], app_id="org.other.App"
        )

        assert extra
        out_mountpoint = path_from_null_term_bytes(extra["mountpoint"])
        assert out_mountpoint == mountpoint

        assert doc_ids
        for doc_id, (file_name, file_content) in zip(doc_ids, files.items()):
            (mountpoint / doc_id / file_name).read_bytes() == file_content
            (Path(os.environ["TMPDIR"]) / file_name).read_bytes() == file_content
            app1_path = mountpoint / "by-app" / "com.test.App1" / doc_id / file_name
            app2_path = mountpoint / "by-app" / "com.test.App2" / doc_id / file_name
            assert not app1_path.exists()
            assert not app2_path.exists()
            assert not (mountpoint / doc_id / "another-file").exists()
            assert not (mountpoint / "anotherid" / file_name).exists()

            other_app_path = (
                mountpoint / "by-app" / "org.other.App" / doc_id / file_name
            )
            assert other_app_path.read_bytes() == file_content
            with pytest.raises(PermissionError):
                other_app_path.write_bytes(b"new-content")

    def test_add_named(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = get_mountpoint(documents_intf)

        content = b"content"
        file_name = "add-named-1"

        folder_path = Path(os.environ["TMPDIR"])
        doc_id = export_file_named(documents_intf, folder_path, file_name)
        assert doc_id

        doc_path = mountpoint / doc_id
        doc_app1_path = mountpoint / "by-app" / "com.test.App1" / doc_id
        doc_app2_path = mountpoint / "by-app" / "com.test.App2" / doc_id

        assert doc_path.exists()
        assert not doc_app1_path.exists()
        assert not (doc_path / file_name).exists()
        assert not (doc_app1_path / file_name).exists()

        documents_intf.GrantPermissions(doc_id, "com.test.App1", ["read", "write"])

        assert doc_path.exists()
        assert doc_app1_path.exists()
        assert not (doc_path / file_name).exists()
        assert not (doc_app1_path / file_name).exists()

        # Update truncating with no previous file
        write_bytes_trunc(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Update truncating with previous file
        content = b"content2"
        write_bytes_trunc(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Update atomic with previous file
        content = b"content3"
        write_bytes_atomic(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Update from host
        content = b"content4"
        write_bytes_atomic(folder_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Unlink doc
        (doc_path / file_name).unlink()
        assert doc_path.exists()
        assert doc_app1_path.exists()
        assert not (doc_path / file_name).exists()
        assert not (doc_app1_path / file_name).exists()

        # Update atomic with no previous file
        content = b"content5"
        write_bytes_atomic(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Unlink doc on host
        (folder_path / file_name).unlink()
        assert doc_path.exists()
        assert doc_app1_path.exists()
        assert not (doc_path / file_name).exists()
        assert not (doc_app1_path / file_name).exists()

        # Update atomic with unexpected no previous file
        content = b"content6"
        write_bytes_atomic(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Unlink doc on host again
        (folder_path / file_name).unlink()
        assert doc_path.exists()
        assert doc_app1_path.exists()
        assert not (doc_path / file_name).exists()
        assert not (doc_app1_path / file_name).exists()

        # Update truncating with unexpected no previous file
        content = b"content7"
        write_bytes_trunc(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

    def test_get_host_paths(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)

        content = b"content"
        file_name = "host-path"

        file_path = Path(os.environ["TMPDIR"]) / file_name
        write_bytes_atomic(file_path, content)
        doc_id = export_file(documents_intf, file_path)

        host_paths = documents_intf.GetHostPaths([doc_id], byte_arrays=True)
        assert doc_id in host_paths
        doc_host_path = path_from_null_term_bytes(host_paths[doc_id])
        assert doc_host_path == file_path


can_run_fuse, failure = xdp.can_run_fuse()
if not can_run_fuse:
    pytest.skip(f"No fuse support: {failure}", allow_module_level=True)
