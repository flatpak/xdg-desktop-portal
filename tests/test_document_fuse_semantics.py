# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import errno
import fcntl
import multiprocessing
import os
import struct
import threading
from pathlib import Path

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


@pytest.fixture
def _tmpfs_file():
    file_path = xdp_doc.make_file("write-data")
    return file_path.parent, file_path.name


@pytest.fixture
def _fuse_file(documents_intf, mountpoint):
    file_path = xdp_doc.make_file("write-data")
    doc_id = xdp_doc.export_file(documents_intf, file_path)
    app_id = "com.test.semantics.File"
    documents_intf.GrantPermissions(doc_id, app_id, ["read", "write"])
    app_path = mountpoint / "by-app" / app_id / doc_id
    return app_path, file_path.name


@pytest.fixture(params=["_tmpfs_file", "_fuse_file"])
def writable_file(request):
    return request.getfixturevalue(request.param)


@pytest.fixture
def _tmpfs_dir():
    return xdp_doc.make_dir()


@pytest.fixture
def _fuse_dir(documents_intf, mountpoint):
    dir_path = xdp_doc.make_dir()
    doc_id = xdp_doc.export_dir(documents_intf, dir_path)
    app_id = "com.test.semantics.Dir"
    documents_intf.GrantPermissions(doc_id, app_id, ["read", "write"])
    return mountpoint / "by-app" / app_id / doc_id / dir_path.name


@pytest.fixture(params=["_tmpfs_dir", "_fuse_dir"])
def writable_dir_path(request):
    return request.getfixturevalue(request.param)


class TestFileSemantics:
    def test_read_content(self, writable_file):
        dirpath, name = writable_file
        assert (dirpath / name).read_text() == "write-data"

    def test_read_multiple_times(self, writable_file):
        dirpath, name = writable_file
        fd = os.open(str(dirpath / name), os.O_RDONLY)
        os.lseek(fd, 0, os.SEEK_SET)
        content = os.read(fd, 4096)
        os.lseek(fd, 0, os.SEEK_SET)
        content2 = os.read(fd, 4096)
        assert content == content2 == b"write-data"
        os.close(fd)

    def test_stat_fields(self, writable_file):
        dirpath, name = writable_file
        info = (dirpath / name).stat()
        assert info.st_uid == os.getuid()
        assert info.st_gid == os.getgid()
        assert info.st_size == len("write-data")

    def test_read_and_write(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        fd = os.open(filepath, os.O_RDONLY)
        os.close(fd)
        fd = os.open(filepath, os.O_RDWR)
        os.close(fd)

    def test_access_checks(self, writable_file):
        dirpath, name = writable_file
        assert os.access(str(dirpath), os.R_OK)
        assert os.access(str(dirpath), os.W_OK)
        assert os.access(str(dirpath / name), os.R_OK)
        assert os.access(str(dirpath / name), os.W_OK)

    def test_rename_and_still_readable_via_fd(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        fd = os.open(filepath, os.O_RDONLY)
        os.rename(filepath, filepath + "_renamed")

        with pytest.raises(FileNotFoundError):
            os.open(filepath, os.O_RDONLY)

        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 4096) == b"write-data"

        os.rename(filepath + "_renamed", filepath)
        os.close(fd)

    def test_delete_and_still_readable_via_fd(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        fd = os.open(filepath, os.O_RDONLY)
        os.unlink(filepath)

        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 4096) == b"write-data"
        os.close(fd)

        Path(filepath).write_text("write-data")

    def test_replace_via_tmpfile_rename(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        tmppath = str(dirpath / "a-tmpfile")

        Path(filepath).write_text("orig-data")
        fd1 = os.open(filepath, os.O_RDONLY)

        Path(tmppath).write_text("new-data")
        fd2 = os.open(tmppath, os.O_RDONLY)

        os.rename(tmppath, filepath)

        os.lseek(fd1, 0, os.SEEK_SET)
        assert os.read(fd1, 4096) == b"orig-data"
        os.lseek(fd2, 0, os.SEEK_SET)
        assert os.read(fd2, 4096) == b"new-data"
        assert Path(filepath).read_text() == "new-data"

        os.close(fd1)
        os.close(fd2)

        Path(filepath).write_text("write-data")

    def test_replace_tmpfile_sequence(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        tmppath = str(dirpath / "a-tmpfile")
        tmppath2 = str(dirpath / "another-tmpfile")

        Path(filepath).write_text("orig-data")
        fd1 = os.open(filepath, os.O_RDONLY)

        Path(tmppath).write_text("new-data")
        fd2 = os.open(tmppath, os.O_RDONLY)

        os.rename(tmppath, tmppath2)
        with pytest.raises(FileNotFoundError):
            os.lstat(tmppath)
        os.lseek(fd2, 0, os.SEEK_SET)
        assert os.read(fd2, 4096) == b"new-data"
        assert Path(tmppath2).read_text() == "new-data"

        os.rename(tmppath2, filepath)
        with pytest.raises(FileNotFoundError):
            os.lstat(tmppath2)
        os.lseek(fd1, 0, os.SEEK_SET)
        assert os.read(fd1, 4096) == b"orig-data"
        os.lseek(fd2, 0, os.SEEK_SET)
        assert os.read(fd2, 4096) == b"new-data"
        assert Path(filepath).read_text() == "new-data"

        os.close(fd1)
        os.close(fd2)

        Path(filepath).write_text("write-data")

    def test_append(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        fd = os.open(filepath, os.O_RDWR)
        os.lseek(fd, 0, os.SEEK_END)
        os.write(fd, b"-more")
        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 4096) == b"write-data-more"
        os.close(fd)

        Path(filepath).write_text("write-data")

    def test_replace_main_file_with_rename(self, writable_file):
        dirpath, name = writable_file
        filepath = str(dirpath / name)
        tmppath = str(dirpath / "a-tmpfile")

        Path(filepath).write_text("new-data-more")
        fd2 = os.open(filepath, os.O_RDONLY)

        Path(tmppath).write_text("replace-this-data")
        fd3 = os.open(tmppath, os.O_RDONLY)

        os.rename(filepath, tmppath)
        os.lseek(fd2, 0, os.SEEK_SET)
        assert os.read(fd2, 4096) == b"new-data-more"
        os.lseek(fd3, 0, os.SEEK_SET)
        assert os.read(fd3, 4096) == b"replace-this-data"

        fd4 = os.open(tmppath, os.O_RDWR)
        os.lseek(fd4, 0, os.SEEK_SET)
        assert os.read(fd4, 4096) == b"new-data-more"

        os.rename(tmppath, filepath)
        os.close(fd2)
        os.close(fd3)
        os.close(fd4)

        Path(filepath).write_text("write-data")

    def test_tmpfile_create_rename_delete(self, writable_file):
        dirpath, _name = writable_file
        tmppath = dirpath / "a-tmpfile"
        tmppath.write_text("tempdata")
        assert tmppath.read_text() == "tempdata"

        tmppath2 = dirpath / "another-tmpfile"
        os.rename(str(tmppath), str(tmppath2))
        assert tmppath2.read_text() == "tempdata"
        tmppath2.unlink()


class TestDirSemantics:
    def test_create_file(self, writable_dir_path):
        filepath = writable_dir_path / "a-file"
        filepath.write_text("filedata")
        assert filepath.read_text() == "filedata"
        filepath.unlink()

    def test_fd_operations(self, writable_dir_path):
        filepath = str(writable_dir_path / "fd-file")
        Path(filepath).write_text("filedata")

        fd = os.open(filepath, os.O_RDONLY)
        fd2 = os.open(filepath, os.O_RDWR)

        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 4096) == b"filedata"

        os.lseek(fd2, 0, os.SEEK_END)
        os.write(fd2, b"-more")

        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 4096) == b"filedata-more"

        os.close(fd)
        os.close(fd2)
        os.unlink(filepath)

    def test_hardlink(self, writable_dir_path):
        subdir = writable_dir_path / "hl-dir"
        os.mkdir(str(subdir))
        filepath = writable_dir_path / "hl-file"
        filepath.write_text("filedata-more")

        linkpath = subdir / "hardlink"
        os.link(str(filepath), str(linkpath))
        assert filepath.stat().st_ino == linkpath.stat().st_ino
        assert linkpath.read_text() == "filedata-more"

        filepath.unlink()
        linkpath.unlink()
        subdir.rmdir()

    def test_symlink(self, writable_dir_path):
        (writable_dir_path / "target").write_text("target")
        os.symlink("target", str(writable_dir_path / "symlink2"))
        assert os.readlink(str(writable_dir_path / "symlink2")) == "target"

        os.symlink("the-void", str(writable_dir_path / "broken-symlink2"))
        assert os.readlink(str(writable_dir_path / "broken-symlink2")) == "the-void"

        os.unlink(str(writable_dir_path / "symlink2"))
        os.unlink(str(writable_dir_path / "broken-symlink2"))
        os.unlink(str(writable_dir_path / "target"))

    def test_move_between_dirs(self, writable_dir_path):
        subdir = writable_dir_path / "mv-dir"
        os.mkdir(str(subdir))
        (subdir / "moveme").write_text("moved")

        os.rename(str(subdir / "moveme"), str(writable_dir_path / "moved"))
        assert (writable_dir_path / "moved").read_text() == "moved"
        assert not (subdir / "moveme").exists()

        os.unlink(str(writable_dir_path / "moved"))
        subdir.rmdir()

    def test_unlink_preserves_fd(self, writable_dir_path):
        filepath = str(writable_dir_path / "del-fd-file")
        Path(filepath).write_text("filedata-more")

        fd = os.open(filepath, os.O_RDONLY)
        fd2 = os.open(filepath, os.O_RDWR)

        linkpath = str(writable_dir_path / "link")
        os.link(filepath, linkpath)

        os.unlink(filepath)
        assert not Path(filepath).exists()

        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 4096) == b"filedata-more"

        os.lseek(fd2, 0, os.SEEK_SET)
        os.ftruncate(fd2, 0)
        os.write(fd2, b"replaced")

        assert Path(linkpath).read_text() == "replaced"

        os.close(fd)
        os.close(fd2)
        os.unlink(linkpath)


class TestParallelAccess:
    NUM_WORKERS = 8

    def _run_parallel(self, fn, args_list):
        errors = []

        def wrapper(*args):
            try:
                fn(*args)
            except Exception as e:  # noqa: BLE001
                errors.append(e)

        threads = []
        for args in args_list:
            t = threading.Thread(target=wrapper, args=args)
            t.start()
            threads.append(t)
        for t in threads:
            t.join()
        if errors:
            raise errors[0]

    def _make_intf(self):
        addr = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        bus = dbus.bus.BusConnection(addr)
        return xdp.get_document_portal_iface(bus)

    def test_concurrent_export_same_file(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("same-file-export")
        results = [None] * self.NUM_WORKERS

        def exporter(idx):
            intf = self._make_intf()
            results[idx] = xdp_doc.export_file(intf, file_path)

        self._run_parallel(exporter, [(i,) for i in range(self.NUM_WORKERS)])
        assert all(r == results[0] for r in results)
        assert (
            mountpoint / results[0] / file_path.name
        ).read_text() == "same-file-export"

    def test_concurrent_export_different_files(self, documents_intf, mountpoint):
        files = [xdp_doc.make_file(f"par-export-{i}") for i in range(self.NUM_WORKERS)]
        results = [None] * self.NUM_WORKERS

        def exporter(idx):
            intf = self._make_intf()
            results[idx] = xdp_doc.export_file(intf, files[idx])

        self._run_parallel(exporter, [(i,) for i in range(self.NUM_WORKERS)])
        assert len(set(results)) == self.NUM_WORKERS

        for idx, doc_id in enumerate(results):
            fuse_path = mountpoint / doc_id / files[idx].name
            assert fuse_path.read_text() == f"par-export-{idx}"

    def test_grant_revoke_while_reading(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("grant-revoke-data")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.parallel.GrantRevoke"
        documents_intf.GrantPermissions(doc_id, app_id, ["read"])
        app_path = mountpoint / "by-app" / app_id / doc_id / file_path.name
        stop = threading.Event()

        def toggler():
            intf = self._make_intf()
            for _ in range(30):
                intf.RevokePermissions(doc_id, app_id, ["read"])
                intf.GrantPermissions(doc_id, app_id, ["read"])
            stop.set()

        def reader():
            while not stop.is_set():
                try:
                    data = app_path.read_text()
                    assert data == "grant-revoke-data"
                except (PermissionError, FileNotFoundError):
                    pass

        threads = [threading.Thread(target=toggler)]
        for _ in range(self.NUM_WORKERS - 1):
            threads.append(threading.Thread(target=reader))
        for t in threads:
            t.start()
        for t in threads:
            t.join()

    def test_concurrent_writes_same_doc(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("contested-write")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        fuse_path = mountpoint / doc_id / file_path.name

        def writer(idx):
            for j in range(20):
                tag = f"w{idx}-{j}"
                xdp_doc.write_bytes_atomic(fuse_path, tag.encode())
                data = fuse_path.read_bytes().decode()
                assert data.startswith("w")

        self._run_parallel(writer, [(i,) for i in range(self.NUM_WORKERS)])
        data = fuse_path.read_bytes().decode()
        assert data.startswith("w")

    def test_read_during_atomic_replace(self, documents_intf, mountpoint):
        file_path = xdp_doc.make_file("replace-target")
        doc_id = xdp_doc.export_file(documents_intf, file_path)
        app_id = "com.test.parallel.Replace"
        documents_intf.GrantPermissions(doc_id, app_id, ["read", "write"])
        app_path = mountpoint / "by-app" / app_id / doc_id
        docpath = app_path / file_path.name
        stop = threading.Event()
        valid_values = {f"version-{j}" for j in range(50)}
        valid_values.add("replace-target")

        def replacer():
            for j in range(50):
                xdp_doc.write_bytes_atomic(docpath, f"version-{j}".encode())
            stop.set()

        def reader():
            while not stop.is_set():
                try:
                    data = docpath.read_text()
                    assert data in valid_values, f"corrupt read: {data!r}"
                except FileNotFoundError:
                    pass

        threads = [threading.Thread(target=replacer)]
        for _ in range(self.NUM_WORKERS - 1):
            threads.append(threading.Thread(target=reader))
        for t in threads:
            t.start()
        for t in threads:
            t.join()


@pytest.fixture
def _tmpfs_lock_file():
    file_path = Path(os.environ["TMPDIR"]) / "lock-test-tmpfile"
    file_path.write_bytes(b"data")
    path = str(file_path)
    return path, path


@pytest.fixture
def _fuse_lock_file(documents_intf, mountpoint):
    file_path = Path(os.environ["TMPDIR"]) / "lock-test-docfile"
    xdp_doc.write_bytes_atomic(file_path, b"data")
    doc_id = xdp_doc.export_file(documents_intf, file_path)
    fuse_path = str(mountpoint / doc_id / "lock-test-docfile")
    return fuse_path, str(file_path)


@pytest.fixture(params=["_tmpfs_lock_file", "_fuse_lock_file"])
def lock_file(request):
    """Each test runs on both a regular file and a FUSE-mounted document.
    The tmpfile variant serves as a baseline: if it fails, the test's
    assumptions about POSIX lock semantics are wrong, not the portal."""
    return request.getfixturevalue(request.param)


class TestDocumentLocking:
    @staticmethod
    def _lock_region(fd, lock_type, start=0, length=0):
        lockdata = struct.pack("hhllll", lock_type, os.SEEK_SET, start, length, 0, 0)
        try:
            fcntl.fcntl(fd, fcntl.F_SETLK, lockdata)
            return True
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EACCES):
                return False
            raise

    @staticmethod
    def _child_try_lock(path, result_queue):
        fd = os.open(path, os.O_RDWR)
        try:
            got_lock = TestDocumentLocking._lock_region(fd, fcntl.F_WRLCK)
            result_queue.put(got_lock)
            result_queue.get()
        finally:
            os.close(fd)

    @staticmethod
    def _child_lock_then_signal(path, locked_event, release_event):
        fd = os.open(path, os.O_RDWR)
        try:
            assert TestDocumentLocking._lock_region(fd, fcntl.F_WRLCK)
            locked_event.set()
            release_event.wait()
        finally:
            os.close(fd)

    @staticmethod
    def _query_lock(fd, start=0, length=0):
        lockdata = struct.pack(
            "hhllll", fcntl.F_WRLCK, os.SEEK_SET, start, length, 0, 0
        )
        result = fcntl.fcntl(fd, fcntl.F_GETLK, lockdata)
        return struct.unpack("hhllll", result)[0]

    def test_exclusive_locks_conflict(self, lock_file):
        lock_path, _ = lock_file

        fd_a = os.open(lock_path, os.O_RDWR)
        try:
            assert self._lock_region(fd_a, fcntl.F_WRLCK)

            result_queue: multiprocessing.Queue[object] = multiprocessing.Queue()
            child = multiprocessing.Process(
                target=self._child_try_lock, args=(lock_path, result_queue)
            )
            child.start()
            try:
                got_lock = result_queue.get(timeout=5)
                assert not got_lock, (
                    "Second process should not acquire a conflicting exclusive lock"
                )
            finally:
                result_queue.put("done")
                child.join(timeout=5)
        finally:
            os.close(fd_a)

    def test_same_process_can_relock_via_different_fds(self, lock_file):
        lock_path, _ = lock_file

        fd_a = os.open(lock_path, os.O_RDWR)
        fd_b = os.open(lock_path, os.O_RDWR)
        try:
            assert self._lock_region(fd_a, fcntl.F_WRLCK)
            got_lock = self._lock_region(fd_b, fcntl.F_WRLCK)
            assert got_lock, "Same process should be able to re-lock via a different fd"
        finally:
            os.close(fd_a)
            os.close(fd_b)

    def test_closing_any_fd_releases_all_locks_for_same_process(self, lock_file):
        lock_path, _ = lock_file

        fd_a = os.open(lock_path, os.O_RDWR)
        fd_b = os.open(lock_path, os.O_RDWR)
        try:
            assert self._lock_region(fd_a, fcntl.F_WRLCK)

            os.close(fd_b)
            fd_b = -1

            # POSIX: closing any fd to a file releases all the process's locks.
            # Verify by having a child process try to acquire the lock.
            result_queue: multiprocessing.Queue[object] = multiprocessing.Queue()
            child = multiprocessing.Process(
                target=self._child_try_lock, args=(lock_path, result_queue)
            )
            child.start()
            try:
                got_lock = result_queue.get(timeout=5)
                assert got_lock, (
                    "Lock should have been released when the other fd was closed"
                )
            finally:
                result_queue.put("done")
                child.join(timeout=5)
        finally:
            os.close(fd_a)
            if fd_b >= 0:
                os.close(fd_b)

    def test_closing_one_fd_does_not_release_other_locks(self, lock_file):
        lock_path, real_path = lock_file

        locked_event = multiprocessing.Event()
        release_event = multiprocessing.Event()

        proc_a = multiprocessing.Process(
            target=self._child_lock_then_signal,
            args=(lock_path, locked_event, release_event),
        )
        proc_a.start()
        try:
            assert locked_event.wait(timeout=5), "Process A failed to acquire lock"

            fd_b = os.open(lock_path, os.O_RDWR)
            os.close(fd_b)

            # Query the lock on the real underlying file since the FUSE
            # mount does not implement F_GETLK.
            check_fd = os.open(real_path, os.O_RDWR)
            try:
                lock_type = self._query_lock(check_fd)
                assert lock_type != fcntl.F_UNLCK, (
                    "Lock should still be held after unrelated fd close"
                )
            finally:
                os.close(check_fd)
        finally:
            release_event.set()
            proc_a.join(timeout=5)


try:
    xdp.ensure_fuse_supported()
except xdp.FuseNotSupportedException as e:
    pytest.skip(f"No fuse support: {e}", allow_module_level=True)
