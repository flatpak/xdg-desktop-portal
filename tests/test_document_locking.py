# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import errno
import fcntl
import multiprocessing
import os
import struct
from pathlib import Path

import pytest

import tests.xdp_doc_utils as xdp_doc
import tests.xdp_utils as xdp


@pytest.fixture
def xdp_app_info() -> xdp.AppInfo:
    return xdp.AppInfoHost(app_id="")


def lock_region(fd, lock_type, start=0, length=0):
    lockdata = struct.pack("hhllll", lock_type, os.SEEK_SET, start, length, 0, 0)
    try:
        fcntl.fcntl(fd, fcntl.F_SETLK, lockdata)
        return True
    except OSError as e:
        if e.errno in (errno.EAGAIN, errno.EACCES):
            return False
        raise


def child_try_lock(path, result_queue):
    fd = os.open(path, os.O_RDWR)
    try:
        got_lock = lock_region(fd, fcntl.F_WRLCK)
        result_queue.put(got_lock)
        result_queue.get()
    finally:
        os.close(fd)


def child_lock_then_signal(path, locked_event, release_event):
    fd = os.open(path, os.O_RDWR)
    try:
        assert lock_region(fd, fcntl.F_WRLCK)
        locked_event.set()
        release_event.wait()
    finally:
        os.close(fd)


def query_lock(fd, start=0, length=0):
    lockdata = struct.pack("hhllll", fcntl.F_WRLCK, os.SEEK_SET, start, length, 0, 0)
    result = fcntl.fcntl(fd, fcntl.F_GETLK, lockdata)
    return struct.unpack("hhllll", result)[0]


@pytest.fixture
def tmpfile():
    file_path = Path(os.environ["TMPDIR"]) / "lock-test-tmpfile"
    file_path.write_bytes(b"data")
    path = str(file_path)
    return path, path


@pytest.fixture
def docfile(xdg_document_portal, dbus_con):
    documents_intf = xdp.get_document_portal_iface(dbus_con)
    mountpoint = xdp_doc.get_mountpoint(documents_intf)

    file_path = Path(os.environ["TMPDIR"]) / "lock-test-docfile"
    xdp_doc.write_bytes_atomic(file_path, b"data")
    doc_id = xdp_doc.export_file(documents_intf, file_path)
    fuse_path = str(mountpoint / doc_id / "lock-test-docfile")
    return fuse_path, str(file_path)


@pytest.fixture(params=["tmpfile", "docfile"])
def test_file(request):
    """Each test runs on both a regular file and a FUSE-mounted document.
    The tmpfile variant serves as a baseline: if it fails, the test's
    assumptions about POSIX lock semantics are wrong, not the portal."""
    return request.getfixturevalue(request.param)


class TestDocumentLocking:
    def test_exclusive_locks_conflict(self, test_file):
        lock_path, _ = test_file

        fd_a = os.open(lock_path, os.O_RDWR)
        try:
            assert lock_region(fd_a, fcntl.F_WRLCK)

            result_queue: multiprocessing.Queue[object] = multiprocessing.Queue()
            child = multiprocessing.Process(
                target=child_try_lock, args=(lock_path, result_queue)
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

    def test_same_process_can_relock_via_different_fds(self, test_file):
        lock_path, _ = test_file

        fd_a = os.open(lock_path, os.O_RDWR)
        fd_b = os.open(lock_path, os.O_RDWR)
        try:
            assert lock_region(fd_a, fcntl.F_WRLCK)
            got_lock = lock_region(fd_b, fcntl.F_WRLCK)
            assert got_lock, "Same process should be able to re-lock via a different fd"
        finally:
            os.close(fd_a)
            os.close(fd_b)

    def test_closing_any_fd_releases_all_locks_for_same_process(self, test_file):
        lock_path, _ = test_file

        fd_a = os.open(lock_path, os.O_RDWR)
        fd_b = os.open(lock_path, os.O_RDWR)
        try:
            assert lock_region(fd_a, fcntl.F_WRLCK)

            os.close(fd_b)
            fd_b = -1

            # POSIX: closing any fd to a file releases all the process's locks.
            # Verify by having a child process try to acquire the lock.
            result_queue: multiprocessing.Queue[object] = multiprocessing.Queue()
            child = multiprocessing.Process(
                target=child_try_lock, args=(lock_path, result_queue)
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

    def test_closing_one_fd_does_not_release_other_locks(self, test_file):
        lock_path, real_path = test_file

        locked_event = multiprocessing.Event()
        release_event = multiprocessing.Event()

        proc_a = multiprocessing.Process(
            target=child_lock_then_signal,
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
                lock_type = query_lock(check_fd)
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
