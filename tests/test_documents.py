# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp
import tests.xdp_doc_utils as xdp_doc

import pytest
import dbus
from pathlib import Path
import os


@pytest.fixture
def xdp_app_info() -> xdp.AppInfo:
    return xdp.AppInfo.new_host(
        app_id="",
    )


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
        xdp_doc.get_mountpoint(documents_intf)

    def test_create_doc(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = xdp_doc.get_mountpoint(documents_intf)

        content = b"content"
        file_name = "a-file"

        file_path = Path(os.environ["TMPDIR"]) / file_name
        xdp_doc.write_bytes_atomic(file_path, content)
        doc_id = xdp_doc.export_file(documents_intf, file_path)

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
        xdp_doc.write_bytes_atomic(doc_path / "tmp1", b"tmpdata1")
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
        xdp_doc.write_bytes_atomic(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert file_path.read_bytes() == content
        assert not (doc_app2_path / file_name).exists()
        assert not (doc_app2_path / "tmp1").exists()

        # Update the document contents outside fuse fd, ensure this is propagated
        content = b"content3"
        xdp_doc.write_bytes_atomic(file_path, content)
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
        xdp_doc.write_bytes_atomic(doc_app1_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert file_path.read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Try to create a tmp file for an app
        assert not (doc_app1_path / "tmp3").exists()
        xdp_doc.write_bytes_atomic(doc_app1_path / "tmp3", b"tmpdata2")
        (doc_app1_path / "tmp3").read_bytes() == b"tmpdata2"
        assert not (doc_path / "tmp3").exists()

        # Re-Create a file from a fuse document file, in various ways
        doc_id2 = xdp_doc.export_file(documents_intf, (doc_path / file_name))
        assert doc_id2 == doc_id
        doc_id3 = xdp_doc.export_file(documents_intf, (doc_app1_path / file_name))
        assert doc_id3 == doc_id
        doc_id4 = xdp_doc.export_file(documents_intf, file_path)
        assert doc_id4 == doc_id

        # Ensure we can make a unique document
        doc_id5 = xdp_doc.export_file(documents_intf, file_path, unique=True)
        assert doc_id5 != doc_id

    def test_recursive_doc(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = xdp_doc.get_mountpoint(documents_intf)

        content = b"content"
        file_name = "recursive-file"

        file_path = Path(os.environ["TMPDIR"]) / file_name
        xdp_doc.write_bytes_atomic(file_path, content)
        doc_id = xdp_doc.export_file(documents_intf, file_path)

        doc_path = mountpoint / doc_id
        doc_app1_path = mountpoint / "by-app" / "com.test.App1" / doc_id

        assert (doc_path / file_name).read_bytes() == content

        doc_id2 = xdp_doc.export_file(documents_intf, doc_path / file_name)
        assert doc_id2 == doc_id

        documents_intf.GrantPermissions(doc_id, "com.test.App1", ["read"])

        doc_id3 = xdp_doc.export_file(documents_intf, doc_app1_path / file_name)
        assert doc_id3 == doc_id

    def test_create_docs(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = xdp_doc.get_mountpoint(documents_intf)

        files = {
            "doc1": b"doc1-content",
            "doc2": b"doc2-content",
        }

        file_paths = []
        for file_name, file_content in files.items():
            file_path = Path(os.environ["TMPDIR"]) / file_name
            xdp_doc.write_bytes_atomic(file_path, file_content)
            file_paths.append(file_path)

        doc_ids, extra = xdp_doc.export_files(
            documents_intf, file_paths, ["read"], app_id="org.other.App"
        )

        assert extra
        out_mountpoint = xdp_doc.path_from_null_term_bytes(extra["mountpoint"])
        assert out_mountpoint == mountpoint

        assert doc_ids
        for doc_id, (file_name, file_content) in zip(doc_ids, files.items()):
            assert (mountpoint / doc_id / file_name).read_bytes() == file_content
            assert (Path(os.environ["TMPDIR"]) / file_name).read_bytes() == file_content
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
        mountpoint = xdp_doc.get_mountpoint(documents_intf)

        content = b"content"
        file_name = "add-named-1"

        folder_path = Path(os.environ["TMPDIR"])
        doc_id = xdp_doc.export_file_named(documents_intf, folder_path, file_name)
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
        xdp_doc.write_bytes_trunc(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Update truncating with previous file
        content = b"content2"
        xdp_doc.write_bytes_trunc(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Update atomic with previous file
        content = b"content3"
        xdp_doc.write_bytes_atomic(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

        # Update from host
        content = b"content4"
        xdp_doc.write_bytes_atomic(folder_path / file_name, content)
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
        xdp_doc.write_bytes_atomic(doc_path / file_name, content)
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
        xdp_doc.write_bytes_atomic(doc_path / file_name, content)
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
        xdp_doc.write_bytes_trunc(doc_path / file_name, content)
        assert (doc_path / file_name).read_bytes() == content
        assert (doc_app1_path / file_name).read_bytes() == content
        assert not (doc_app2_path / file_name).exists()

    def test_get_host_paths(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)

        content = b"content"
        file_name = "host-path"

        file_path = Path(os.environ["TMPDIR"]) / file_name
        xdp_doc.write_bytes_atomic(file_path, content)
        doc_id = xdp_doc.export_file(documents_intf, file_path)

        host_paths = documents_intf.GetHostPaths([doc_id], byte_arrays=True)
        assert doc_id in host_paths
        doc_host_path = xdp_doc.path_from_null_term_bytes(host_paths[doc_id])
        assert doc_host_path == file_path

    def test_host_paths_xattr(self, xdg_document_portal, dbus_con):
        documents_intf = xdp.get_document_portal_iface(dbus_con)
        mountpoint = xdp_doc.get_mountpoint(documents_intf)

        base_path = Path(os.environ["TMPDIR"]) / "a"
        file_path = base_path / "b" / "c"
        file_path.parent.mkdir(parents=True, exist_ok=True)
        file_path.write_bytes(b"test")

        doc_ids, extra = xdp_doc.export_files(
            documents_intf,
            [base_path],
            ["read"],
            flags=xdp_doc.EXPORT_FILES_FLAG_EXPORT_DIR,
        )
        doc_id = doc_ids[0]

        host_path = xdp_doc.get_host_path_attr(mountpoint / doc_id)
        assert not host_path

        host_path = xdp_doc.get_host_path_attr(mountpoint / doc_id / "a")
        assert host_path == base_path

        host_path = xdp_doc.get_host_path_attr(mountpoint / doc_id / "a" / "b")
        assert host_path == base_path / "b"

        host_path = xdp_doc.get_host_path_attr(mountpoint / doc_id / "a" / "b" / "c")
        assert host_path == base_path / "b" / "c"


try:
    xdp.ensure_fuse_supported()
except xdp.FuseNotSupportedException as e:
    pytest.skip(f"No fuse support: {e}", allow_module_level=True)
