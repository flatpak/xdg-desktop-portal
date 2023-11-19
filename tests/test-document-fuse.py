#!/usr/bin/env python3

# Copyright © 2020 Red Hat, Inc
# Copyright © 2023 GNOME Foundation Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <http://www.gnu.org/licenses/>.
#
# Authors:
#       Alexander Larsson <alexl@redhat.com>
#       Hubert Figuière <hub@figuiere.net>

import argparse
import errno
import os
import random
import stat
import sys

from gi.repository import Gio, GLib


def filename_to_ay(filename):
    return list(filename.encode("utf-8")) + [0]


running_count = {}

app_prefix = "org.test."
dir_prefix = "dir"
ensure_no_remaining = True

parser = argparse.ArgumentParser()
parser.add_argument("--verbose", "-v", action="count")
parser.add_argument("--iterations", type=int, default=3)
parser.add_argument("--prefix")
args = parser.parse_args(sys.argv[1:])

if args.prefix:
    app_prefix = app_prefix + args.prefix + "."
    dir_prefix = dir_prefix + "-" + args.prefix + "-"
    ensure_no_remaining = False


def log(str):
    if args.prefix:
        print("%s: %s" % (args.prefix, str), file=sys.stderr)
    else:
        print(str, file=sys.stderr)


def logv(str):
    if args.verbose:
        log(str)


def get_a_count(counter):
    global running_count
    if counter in running_count:
        count = running_count[counter]
        count = count + 1
        running_count[counter] = count
        return count
    running_count[counter] = 1
    return 1


def setFileContent(path, content):
    with open(path, "w") as f:
        f.write(content)


def appendFileContent(path, content):
    with open(path, "a") as f:
        f.write(content)


def readFdContent(fd):
    os.lseek(fd, 0, os.SEEK_SET)
    return str(os.read(fd, 64 * 1024), "utf-8")


def replaceFdContent(fd, content):
    os.lseek(fd, 0, os.SEEK_SET)
    os.ftruncate(fd, 0)
    os.write(fd, bytes(content, "utf-8"))


def appendFdContent(fd, content):
    os.lseek(fd, 0, os.SEEK_END)
    os.write(fd, bytes(content, "utf-8"))


TEST_DATA_DIR = os.environ["TEST_DATA_DIR"]
DOCUMENT_ADD_FLAGS_REUSE_EXISTING = 1 << 0
DOCUMENT_ADD_FLAGS_PERSISTENT = 1 << 1
DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP = 1 << 2
DOCUMENT_ADD_FLAGS_DIRECTORY = 1 << 3


def assertRaises(exc_type, func, *args, **kwargs):
    raised_exc = None
    try:
        func(*args, **kwargs)
    except:
        raised_exc = sys.exc_info()[0]

    if not raised_exc:
        raise AssertionError("{0} was not raised".format(exc_type.__name__))
    if raised_exc != exc_type:
        raise AssertionError(
            "Wrong assertion type {0} was raised instead of {1}".format(
                raised_exc.__name__, exc_type.__name__
            )
        )


def assertRaisesErrno(error_nr, func, *args, **kwargs):
    raised_exc = None
    raised_exc_value = None
    try:
        func(*args, **kwargs)
    except:
        raised_exc = sys.exc_info()[0]
        raised_exc_value = sys.exc_info()[1]

    if not raised_exc:
        raise AssertionError("No assertion was raised")
    if raised_exc != OSError:
        raise AssertionError("OSError was not raised")
    if raised_exc_value.errno != error_nr:
        raise AssertionError(
            "Wrong errno {0} was raised instead of {1}".format(
                raised_exc_value.errno, error_nr
            )
        )


def assertRaisesGError(message, code, func, *args, **kwargs):
    raised_exc = None
    raised_exc_value = None
    try:
        func(*args, **kwargs)
    except:
        raised_exc = sys.exc_info()[0]
        raised_exc_value = sys.exc_info()[1]

    if not raised_exc:
        raise AssertionError("No assertion was raised")
    if raised_exc != GLib.GError:
        raise AssertionError("GError was not raised")
    if not raised_exc_value.message.startswith(message):
        raise AssertionError(
            "Wrong message {0} doesn't start with {1}".format(
                raised_exc_value.message, message
            )
        )
    if raised_exc_value.code != code:
        raise AssertionError(
            "Wrong code {0} was raised instead of {1}".format(
                raised_exc_value.code, code
            )
        )


def assertFileHasContent(path, expected_content):
    with open(path) as f:
        file_content = f.read()
        assert file_content == expected_content


def assertFdHasContent(fd, expected_content):
    content = readFdContent(fd)
    assert content == expected_content


def assertSameStat(a, b, b_mode_mask):
    if not (
        a.st_mode == (b.st_mode & b_mode_mask)
        and a.st_nlink == b.st_nlink
        and a.st_size == b.st_size
        and a.st_uid == b.st_uid
        and a.st_gid == b.st_gid
        and a.st_atime == b.st_atime
        and a.st_mtime == b.st_mtime
        and a.st_ctime == b.st_ctime
    ):
        raise AssertionError("Stat value {} was not the expected {})".format(a, b))


def assertFileExist(path):
    try:
        info = os.lstat(path)
        if info.st_mode & stat.S_IFREG != stat.S_IFREG:
            raise AssertionError("File {} is not a regular file".format(path))
    except:
        raise AssertionError("File {} doesn't exist".format(path))


def assertDirExist(path):
    try:
        info = os.lstat(path)
        if info.st_mode & stat.S_IFDIR != stat.S_IFDIR:
            raise AssertionError("File {} is not a directory file".format(path))
    except:
        raise AssertionError("File {} doesn't exist".format(path))


def assertSymlink(path, expected_target):
    try:
        info = os.lstat(path)
        if info.st_mode & stat.S_IFLNK != stat.S_IFLNK:
            raise AssertionError("File {} is not a symlink".format(path))
        target = os.readlink(path)
        if target != expected_target:
            raise AssertionError(
                "File {} has wrong target {}, expected {}".format(
                    path, target, expected_target
                )
            )
    except:
        raise AssertionError("Symlink {} doesn't exist".format(path))


def assertFileNotExist(path):
    try:
        os.lstat(path)
    except FileNotFoundError:
        return
    except:
        raise AssertionError(
            "Got wrong execption {} for {}, expected FileNotFoundError".format(
                sys.exc_info()[0], path
            )
        )
    raise AssertionError("Path {} unexpectedly exists".format(path))


def assertDirFiles(path, expected_files, exhaustive=True, volatile_files=None):
    found_files = os.listdir(path)
    remaining = set(found_files)
    for file in expected_files:
        if file in remaining:
            remaining.remove(file)
        elif file not in volatile_files:
            raise AssertionError(
                "Expected file {} not found in dir {} (all: {})".format(
                    file, path, found_files
                )
            )
    if exhaustive:
        if len(remaining) != 0:
            raise AssertionError(
                "Unexpected files {} in dir {} (all: {})".format(
                    remaining, path, found_files
                )
            )


class Doc:
    def __init__(self, portal, id, path, content, is_dir=False):
        self.portal = portal
        self.id = id
        self.content = content
        self.real_path = path
        self.is_dir = is_dir
        self.apps = []
        self.files = []

        if is_dir:
            self.real_dirname = path
            self.filename = None
            self.dirname = os.path.basename(path)
        else:
            (self.real_dirname, self.filename) = os.path.split(path)
            self.dirname = None
            if content:
                self.files.append(self.filename)

    def is_readable_by(self, app_id):
        if app_id:
            return app_id in self.apps
        return True

    def is_writable_by(self, app_id):
        if app_id:
            return app_id in self.apps and ".write." in app_id
        else:
            return True

    def get_doc_path(self, app_id):
        if app_id:
            base = self.portal.app_path(app_id) + "/" + self.id
        else:
            base = self.portal.mountpoint + "/" + self.id
        if self.is_dir:
            return base + "/" + self.dirname
        else:
            return base

    def __str__(self):
        name = self.id
        if self.is_dir:
            return "%s(dir)" % (name)
        elif self.content is None:
            return "%s(missing)" % (name)
        else:
            return "%s" % (name)


class DocPortal:
    def __init__(self):
        self.apps = []
        self.volatile_apps = set()
        self.docs = {}
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.proxy = Gio.DBusProxy.new_sync(
            self.bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.portal.Documents",
            "/org/freedesktop/portal/documents",
            "org.freedesktop.portal.Documents",
            None,
        )
        self.mountpoint = self.get_mount_path()

    def get_mount_path(self):
        res = self.proxy.call_sync("GetMountPoint", GLib.Variant("()", ()), 0, -1, None)
        return bytearray(res[0][:-1]).decode("utf-8")

    def grant_permissions(self, doc_id, app_id, permissions):
        self.proxy.call_sync(
            "GrantPermissions",
            GLib.Variant("(ssas)", (doc_id, app_id, permissions)),
            0,
            -1,
            None,
        )

    def lookup(self, path):
        res = self.proxy.call_sync(
            "Lookup", GLib.Variant("(ay)", (filename_to_ay(path),)), 0, -1, None
        )
        return res[0]

    def delete(self, doc_id):
        self.proxy.call_sync("Delete", GLib.Variant("(s)", (doc_id,)), 0, -1, None)
        del self.docs[doc_id]

    def add(self, path, reuse_existing=True):
        fdlist = Gio.UnixFDList.new()
        fd = os.open(path, os.O_PATH)
        handle = fdlist.append(fd)
        os.close(fd)
        res = self.proxy.call_with_unix_fd_list_sync(
            "Add",
            GLib.Variant("(hbb)", (handle, reuse_existing, False)),
            0,
            -1,
            fdlist,
            None,
        )
        doc_id = res[0][0]
        if doc_id in self.docs:
            return self.docs[doc_id]

        with open(path) as f:
            content = f.read()
        doc = Doc(self, doc_id, path, content)
        self.docs[doc.id] = doc
        return doc

    def add_named(self, path, reuse_existing=True):
        (dirname, filename) = os.path.split(path)
        fdlist = Gio.UnixFDList.new()
        fd = os.open(dirname, os.O_PATH)
        handle = fdlist.append(fd)
        os.close(fd)
        res = self.proxy.call_with_unix_fd_list_sync(
            "AddNamed",
            GLib.Variant(
                "(haybb)", (handle, filename_to_ay(filename), reuse_existing, False)
            ),
            0,
            -1,
            fdlist,
            None,
        )
        doc_id = res[0][0]
        if doc_id in self.docs:
            return self.docs[doc_id]

        try:
            with open(path) as f:
                content = f.read()
        except:
            content = None
        doc = Doc(self, doc_id, path, content)
        self.docs[doc.id] = doc
        return doc

    def add_full(self, path, flags):
        fdlist = Gio.UnixFDList.new()
        fd = os.open(path, os.O_PATH)
        handle = fdlist.append(fd)
        os.close(fd)
        res = self.proxy.call_with_unix_fd_list_sync(
            "AddFull",
            GLib.Variant("(ahusas)", ([handle], flags, "", [])),
            0,
            -1,
            fdlist,
            None,
        )
        doc_id = res[0][0][0]
        if doc_id in self.docs:
            return self.docs[doc_id]
        doc = Doc(self, doc_id, path, True, (flags & DOCUMENT_ADD_FLAGS_DIRECTORY) != 0)
        self.docs[doc.id] = doc
        return doc

    def add_dir(self, path):
        return self.add_full(
            path, DOCUMENT_ADD_FLAGS_REUSE_EXISTING | DOCUMENT_ADD_FLAGS_DIRECTORY
        )

    def get_docs_for_app(self, app_id):
        docs = []
        for doc in self.docs.values():
            if doc.is_readable_by(app_id):
                docs.append(doc.id)
        return docs

    def ensure_app_id(self, app_id, volatile=False):
        if app_id not in self.apps:
            self.apps.append(app_id)
        if volatile:
            self.volatile_apps.add(app_id)

    def get_docs(self):
        return list(self.docs.values())

    def get_docs_randomized(self):
        docs = list(self.docs.values())
        random.shuffle(docs)
        return docs

    def get_doc(self, doc_id):
        return self.docs[doc_id]

    def get_app_ids(self):
        return self.apps

    def get_volatile_app_ids(self):
        return self.volatile_apps

    def get_app_ids_randomized(self):
        apps = self.apps.copy()
        random.shuffle(apps)
        return apps

    def by_app_path(self):
        return self.mountpoint + "/by-app"

    def app_path(self, app_id):
        return self.mountpoint + "/by-app/" + app_id

class FileTransferPortal(DocPortal):
    def __init__(self):
        super().__init__()
        self.ft_proxy = Gio.DBusProxy.new_sync(
            self.bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.portal.Documents",
            "/org/freedesktop/portal/documents",
            "org.freedesktop.portal.FileTransfer",
            None,
        )

    def start_transfer(self):
        res = self.ft_proxy.call_sync("StartTransfer", GLib.Variant("(a{sv})", ([None])), 0, -1, None)
        return res[0]

    def add_files(self, key, files):
        fdlist = Gio.UnixFDList.new()
        handles = []
        for filename in files:
            fd = os.open(filename, os.O_PATH)
            handle = fdlist.append(fd)
            handles.append(handle)
            os.close(fd)

        res = self.ft_proxy.call_with_unix_fd_list_sync(
            "AddFiles",
            GLib.Variant("(saha{sv})", (key, handles, [])),
            0,
            -1,
            fdlist,
            None,
        )
        return res

    def retrieve_files(self, key):
        res = self.ft_proxy.call_sync(
            "RetrieveFiles",
            GLib.Variant("(sa{sv})", (key, [])),
            0,
            -1,
            None,
        )
        return res

    def stop_transfer(self, key):
        res = self.ft_proxy.call_sync(
            "StopTransfer",
            GLib.Variant("(s)", (key,)),
            0,
            -1,
            None,
        )
        return res

def check_virtual_stat(info, writable=False):
    assert info.st_uid == os.getuid()
    assert info.st_gid == os.getgid()
    if writable:
        assert info.st_mode == stat.S_IFDIR | 0o700
    else:
        assert info.st_mode == stat.S_IFDIR | 0o500


def verify_virtual_dir(path, files, volatile_files=None):
    info = os.lstat(path)
    check_virtual_stat(info)
    assert os.access(path, os.R_OK)
    assert not os.access(path, os.W_OK)

    assertRaises(FileNotFoundError, os.lstat, path + "/not-existing-file")

    if files is not None:
        assertDirFiles(path, files, ensure_no_remaining, volatile_files)


def verify_doc(doc, app_id=None):
    dir = doc.get_doc_path(app_id)

    if doc.is_dir:
        vdir = os.path.dirname(dir)
        info = os.lstat(vdir)
        check_virtual_stat(info)
        pass
    else:
        info = os.lstat(dir)
        check_virtual_stat(info, doc.is_writable_by(app_id))
        assert os.access(dir, os.R_OK)
        if doc.is_writable_by(app_id):
            assert os.access(dir, os.W_OK)
        else:
            assert not os.access(dir, os.W_OK)

    assertRaises(FileNotFoundError, os.lstat, dir + "/not-existing-file")

    assertDirFiles(dir, doc.files)

    for file in doc.files:
        filepath = dir + "/" + file
        info = os.lstat(filepath)
        assert info.st_uid == os.getuid()
        assert info.st_gid == os.getgid()

        assert os.access(filepath, os.R_OK)
        if doc.is_writable_by(app_id):
            assert os.access(filepath, os.W_OK)
        else:
            assert not os.access(filepath, os.W_OK)

    if doc.filename:
        main_path = dir + "/" + doc.filename
        real_path = doc.real_path
        if doc.content:
            assertFileExist(main_path)
            assertFileHasContent(main_path, doc.content)
            assertFileHasContent(real_path, doc.content)

            info = os.lstat(main_path)
            real_info = os.lstat(real_path)
            mode_mask = ~(stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX)
            if not doc.is_writable_by(app_id):
                mode_mask = mode_mask & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
            assertSameStat(info, real_info, mode_mask)

        else:
            assertRaises(FileNotFoundError, os.lstat, main_path)
            assertRaises(FileNotFoundError, os.open, main_path, os.O_RDONLY)
            assertRaises(FileNotFoundError, os.lstat, doc.real_path)
            assertRaises(FileNotFoundError, os.open, doc.real_path, os.O_RDONLY)

    # Ensure no leftover temp files
    for real_file in os.listdir(os.path.dirname(doc.real_path)):
        assert not real_file.startswith(".xdp")


def verify_fs_layout(portal):
    verify_virtual_dir(portal.mountpoint, ["by-app"] + list(portal.docs.keys()))
    verify_virtual_dir(
        portal.by_app_path(), portal.get_app_ids(), portal.get_volatile_app_ids()
    )

    for doc in portal.get_docs():
        verify_doc(doc)

    # Verify the by-app subdirs (just the directory for now)
    for app_id in portal.get_app_ids():
        docs_for_app = portal.get_docs_for_app(app_id)
        verify_virtual_dir(portal.app_path(app_id), docs_for_app)
        for doc_id in docs_for_app:
            doc = portal.get_doc(doc_id)
            verify_doc(doc, app_id)


def check_virtdir_perms(path):
    assertRaises(PermissionError, os.mkdir, path + "/a_dir")
    assertRaises(PermissionError, os.open, path + "/a-file", os.O_RDWR | os.O_CREAT)


def check_root_perms(path):
    check_virtdir_perms(path)
    assertRaises(PermissionError, os.rename, path + "/by-app", path + "/by-app2")
    assertRaises(PermissionError, os.rmdir, path + "/by-app")


def check_byapp_perms(path):
    check_virtdir_perms(path)
    assertRaises(PermissionError, os.mkdir, path + "/a_dir")


def check_regular_doc_perms(doc, app_id):
    path = doc.get_doc_path(app_id)
    writable = doc.is_writable_by(app_id)
    # regular documents, can't do most stuff
    assertRaises(PermissionError, os.mkdir, path + "/dir")
    assertRaises(PermissionError, os.symlink, "symlink-value", path + "/symlink")

    docpath = path + "/" + doc.filename
    tmppath = path + "/a-tmpfile"
    tmppath2 = path + "/another-tmpfile"
    if doc.content:  # Main file exists
        assertFileExist(docpath)
        assertFileExist(doc.real_path)
        assertRaises(PermissionError, os.link, docpath, path + "/a-hardlink")
        assertRaises(NotADirectoryError, os.rmdir, docpath)
        assertRaises(PermissionError, os.setxattr, docpath, "user.attr", b"foo")
        assertRaises(PermissionError, os.removexattr, docpath, "user.attr")

        fd = os.open(docpath, os.O_RDONLY, 0o600)
        os.close(fd)

        if not writable:
            assertRaises(
                PermissionError, os.open, docpath, os.O_RDONLY | os.O_TRUNC, 0o600
            )
            assertRaises(PermissionError, os.open, docpath, os.O_WRONLY, 0o600)
            assertRaises(PermissionError, os.open, docpath, os.O_RDWR, 0o600)
            assertRaises(PermissionError, os.rename, docpath, docpath + "renamed")
            assertRaises(PermissionError, os.truncate, docpath, 1)
            assertRaises(PermissionError, os.unlink, docpath)
            assertRaises(PermissionError, os.utime, docpath)
        else:
            # Can't move file out of docdir or into other version of same docdir
            assertRaisesErrno(
                errno.EXDEV, os.rename, docpath, path + "/../" + doc.filename
            )
            if app_id:
                assertRaisesErrno(
                    errno.EXDEV,
                    os.rename,
                    docpath,
                    doc.get_doc_path(None) + doc.filename,
                )
            if doc.apps and app_id != doc.apps[0]:
                assertRaisesErrno(
                    errno.EXDEV,
                    os.rename,
                    docpath,
                    doc.get_doc_path(doc.apps[0]) + doc.filename,
                )

            # Ensure we can read it (multiple times)
            fd = os.open(docpath, os.O_RDONLY, 0o600)
            assertFdHasContent(fd, doc.content)
            assertFdHasContent(fd, doc.content)

            # Ensure we can rename it
            os.rename(docpath, docpath + "_renamed")
            assertRaises(FileNotFoundError, os.open, docpath, os.O_RDONLY, 0o600)
            # ... and still read it
            assertFdHasContent(fd, doc.content)

            # Ensure we can delete it
            os.unlink(docpath + "_renamed")
            # ... and still read it
            assertFdHasContent(fd, doc.content)
            os.close(fd)

            # Replace main file with rename of tmpfile
            setFileContent(docpath, "orig-data")
            fd1 = os.open(docpath, os.O_RDONLY, 0o600)

            setFileContent(tmppath, "new-data")
            fd2 = os.open(tmppath, os.O_RDONLY, 0o600)

            os.rename(tmppath, tmppath2)
            assertRaises(FileNotFoundError, os.lstat, tmppath)
            assertFdHasContent(fd2, "new-data")
            assertFileHasContent(tmppath2, "new-data")

            os.rename(tmppath2, docpath)
            assertRaises(FileNotFoundError, os.lstat, tmppath2)
            assertFdHasContent(fd1, "orig-data")
            assertFdHasContent(fd2, "new-data")
            assertFileHasContent(docpath, "new-data")
            appendFileContent(docpath, "-more")
            assertFdHasContent(fd2, "new-data-more")

            setFileContent(tmppath, "replace-this-data")
            fd3 = os.open(tmppath, os.O_RDONLY, 0o600)
            os.rename(docpath, tmppath)
            assertFdHasContent(fd2, "new-data-more")
            assertFdHasContent(fd3, "replace-this-data")
            fd4 = os.open(tmppath, os.O_RDWR, 0o600)
            assertFdHasContent(fd4, "new-data-more")

            # Restore original version
            os.rename(tmppath, docpath)
            replaceFdContent(fd4, doc.content)
            assertFdHasContent(fd2, doc.content)
            assertFdHasContent(fd4, doc.content)
            assertFileHasContent(docpath, doc.content)
            assertFdHasContent(fd1, "orig-data")
            assertFdHasContent(fd3, "replace-this-data")

            os.close(fd1)
            os.close(fd2)
            os.close(fd3)
            os.close(fd4)

            assertRaises(NotADirectoryError, os.rmdir, docpath)
            assertRaises(PermissionError, os.link, docpath, path + "/a-hardlink")
            assertRaises(PermissionError, os.setxattr, docpath, "user.attr", b"foo")
            assertRaises(PermissionError, os.removexattr, docpath, "user.attr")

    else:  # Main file doesn't exist
        assertFileNotExist(docpath)
        assertFileNotExist(doc.real_path)
        if writable:  # But we can create it
            setFileContent(docpath, "some-data")
            assertFileHasContent(docpath, "some-data")
            os.unlink(docpath)
        else:  # And we can't create it
            assertRaises(
                PermissionError,
                os.open,
                docpath,
                os.O_CREAT | os.O_RDONLY | os.O_TRUNC,
                0o600,
            )
            assertRaises(
                PermissionError, os.open, docpath, os.O_CREAT | os.O_WRONLY, 0o600
            )
            assertRaises(
                PermissionError, os.open, docpath, os.O_CREAT | os.O_RDWR, 0o600
            )

        # Ensure it show up if created outside
        setFileContent(doc.real_path, "from-outside")
        assertFileExist(docpath)
        assertFileHasContent(docpath, "from-outside")
        if writable:
            os.unlink(docpath)
        else:
            assertRaises(PermissionError, os.unlink, docpath)
            os.unlink(doc.real_path)
        assertFileNotExist(docpath)

    if writable:  # We can create tempfiles, do some simple checks
        setFileContent(tmppath, "tempdata")
        assertFileHasContent(tmppath, "tempdata")
        assertRaises(NotADirectoryError, os.rmdir, tmppath)
        assertRaises(PermissionError, os.link, tmppath, path + "/a-hardlink")
        assertRaises(PermissionError, os.setxattr, tmppath, "user.attr", b"foo")
        assertRaises(PermissionError, os.removexattr, tmppath, "user.attr")

        os.rename(tmppath, tmppath2)
        assertFileHasContent(tmppath2, "tempdata")
        os.unlink(tmppath2)
    else:
        # We should be unable to create tempfiles
        assertRaises(
            PermissionError,
            os.open,
            tmppath,
            os.O_CREAT | os.O_RDONLY | os.O_TRUNC,
            0o600,
        )
        assertRaises(PermissionError, os.open, tmppath, os.O_CREAT | os.O_WRONLY, 0o600)
        assertRaises(PermissionError, os.open, tmppath, os.O_CREAT | os.O_RDWR, 0o600)


def check_directory_doc_perms(doc, app_id):
    writable = doc.is_writable_by(app_id)

    docpath = doc.get_doc_path(app_id)
    realpath = doc.real_path

    # We should not be able to do anything with the toplevel document dir (other than reading the real dir)
    vpath = os.path.dirname(docpath)
    assertDirExist(vpath)
    assertDirFiles(vpath, [doc.dirname])
    assertRaises(PermissionError, os.mkdir, vpath + "/a_dir")
    assertRaises(PermissionError, os.rename, docpath, vpath + "/foo")
    assertRaises(PermissionError, os.rmdir, docpath)
    assertRaises(
        PermissionError, os.open, vpath + "/a_file", os.O_CREAT | os.O_RDWR, 0o600
    )

    assertDirExist(docpath)

    # Create some pre-existing files:

    real_dir = realpath + "/dir"
    os.mkdir(real_dir)
    setFileContent(real_dir + "/realfile", "real1")
    setFileContent(real_dir + "/readonly", "readonly")
    os.chmod(real_dir + "/readonly", 0o500)
    os.mkdir(real_dir + "/subdir")
    os.link(real_dir + "/realfile", real_dir + "/subdir/hardlink")
    os.symlink("realfile", real_dir + "/symlink")
    os.symlink("the-void", real_dir + "/broken-symlink")

    # Ensure they are visible via portal

    dir = docpath + "/dir"
    assertDirFiles(docpath, ["dir"])
    assertDirExist(dir)
    assertDirFiles(dir, ["realfile", "readonly", "subdir", "symlink", "broken-symlink"])
    assertDirExist(dir + "/subdir")
    assertDirFiles(dir + "/subdir", ["hardlink"])
    assertFileHasContent(dir + "/realfile", "real1")
    assertFileHasContent(dir + "/readonly", "readonly")
    assertFileHasContent(dir + "/subdir/hardlink", "real1")
    assert (
        os.lstat(dir + "/realfile").st_ino == os.lstat(dir + "/subdir/hardlink").st_ino
    )
    assertSymlink(dir + "/symlink", "realfile")
    assertSymlink(dir + "/broken-symlink", "the-void")

    filepath = docpath + "/a-file"
    real_filepath = doc.real_path + "/a-file"
    filepath2 = docpath + "/dir/a-file2"
    real_filepath2 = doc.real_path + "/dir/a-file2"

    if writable:  # We can create files
        if os.environ.get("TEST_IN_ROOTED_CI"):
            assertRaises(PermissionError, os.open, dir + "/readonly", os.O_RDWR)
            os.chmod(dir + "/readonly", 0o700)
            fd = os.open(dir + "/readonly", os.O_RDWR)  # Works now
            os.close(fd)

        setFileContent(filepath, "filedata")
        assertFileHasContent(filepath, "filedata")
        assertFileHasContent(real_filepath, "filedata")

        fd = os.open(filepath, os.O_RDONLY)
        fd2 = os.open(filepath, os.O_RDWR)
        assertFdHasContent(fd, "filedata")
        assertFdHasContent(fd2, "filedata")
        appendFdContent(fd2, "-more")
        assertFdHasContent(fd, "filedata-more")
        assertFdHasContent(fd2, "filedata-more")

        os.link(filepath, filepath2)
        assert os.lstat(filepath).st_ino == os.lstat(filepath2).st_ino
        assert os.lstat(filepath).st_ino == os.fstat(fd).st_ino
        assertFileHasContent(filepath2, "filedata-more")
        assertFileHasContent(real_filepath2, "filedata-more")

        os.unlink(filepath)
        assertFileNotExist(filepath)
        assertFileNotExist(real_filepath)
        assertFdHasContent(fd, "filedata-more")
        assertFdHasContent(fd2, "filedata-more")

        replaceFdContent(fd2, "replaced")
        assertFileHasContent(filepath2, "replaced")
        assertFileHasContent(real_filepath2, "replaced")
        assertFileNotExist(filepath)
        assertFileNotExist(real_filepath)
        assertFdHasContent(fd, "replaced")
        assertFdHasContent(fd2, "replaced")

        # Move between dirs
        os.rename(filepath2, docpath + "/moved")
        assertFileHasContent(docpath + "/moved", "replaced")

        assertRaisesErrno(errno.EXDEV, os.rename, docpath, doc.portal.mountpoint)

        os.unlink(docpath + "/moved")

        os.close(fd)
        os.close(fd2)

        os.symlink("realfile", dir + "/symlink2")
        os.symlink("the-void", dir + "/broken-symlink2")
        assertSymlink(dir + "/symlink2", "realfile")
        assertSymlink(dir + "/broken-symlink2", "the-void")
        os.unlink(dir + "/symlink2")
        os.unlink(dir + "/broken-symlink2")

    else:
        # We should be unable to create files
        assertRaises(
            PermissionError,
            os.open,
            filepath,
            os.O_CREAT | os.O_RDONLY | os.O_TRUNC,
            0o600,
        )
        assertRaises(
            PermissionError, os.open, filepath, os.O_CREAT | os.O_WRONLY, 0o600
        )
        assertRaises(PermissionError, os.open, filepath, os.O_CREAT | os.O_RDWR, 0o600)

        assertRaises(PermissionError, os.open, dir + "/realfile", os.O_RDWR)
        assertRaises(PermissionError, os.open, dir + "/readonly", os.O_RDWR)
        assertRaises(PermissionError, os.truncate, dir + "/realfile", 0)
        assertRaises(PermissionError, os.link, dir + "/realfile", dir + "/foo")
        assertRaises(PermissionError, os.symlink, "foo", dir + "/new-symlink")
        assertRaises(PermissionError, os.rename, dir + "/realfile", dir + "/foo")
        assertRaises(PermissionError, os.unlink, dir + "/realfile")
        assertRaises(PermissionError, os.chmod, dir + "/realfile", 0o700)
        assertRaises(PermissionError, os.rmdir, dir + "/subdir")

    os.unlink(real_dir + "/realfile")
    os.unlink(real_dir + "/readonly")
    os.unlink(real_dir + "/subdir/hardlink")
    os.unlink(real_dir + "/symlink")
    os.unlink(real_dir + "/broken-symlink")
    os.rmdir(real_dir + "/subdir")
    os.rmdir(real_dir)


def check_doc_perms(doc, app_id):
    path = doc.get_doc_path(app_id)
    readable = doc.is_readable_by(app_id)
    if not readable:
        assertRaises(FileNotFoundError, os.lstat, path)
        if doc.is_dir:  # Non readable dir means we can't even see the toplevel dir
            assertRaises(FileNotFoundError, os.mkdir, path)
        else:
            assertRaises(PermissionError, os.mkdir, path)
        return

    assertRaises(PermissionError, os.rmdir, path)
    assertRaises(PermissionError, os.rename, path, path + "_renamed")
    assertRaises(IsADirectoryError, os.unlink, path)
    if doc.is_dir:
        check_directory_doc_perms(doc, app_id)
    else:
        check_regular_doc_perms(doc, app_id)


def check_perms(portal):
    check_root_perms(portal.mountpoint)
    check_byapp_perms(portal.by_app_path())

    for doc in portal.get_docs_randomized():
        check_doc_perms(doc, None)
        for app_id in portal.get_app_ids_randomized():
            check_doc_perms(doc, app_id)


# Ensure that a single lookup by app-id creates that app id (we need this for when mounting the subdir for an app)
def create_app_by_lookup(portal):
    # Should only work for valid app ids
    assertRaises(FileNotFoundError, os.lstat, portal.app_path("not-an-app-id"))

    app_id = app_prefix + "Lookup"
    info = os.lstat(portal.app_path(app_id))
    check_virtual_stat(info)
    portal.ensure_app_id(app_id, volatile=True)


def ensure_real_dir(create_hidden_file=True):
    count = get_a_count("doc")
    dir = TEST_DATA_DIR + "/" + dir_prefix + str(count)
    os.makedirs(dir)
    if create_hidden_file:
        setFileContent(dir + "/cant-see-this-file", "s3krit")
    return (dir, count)


def ensure_real_dir_file(create_file):
    (dir, count) = ensure_real_dir()
    path = dir + "/the-file"
    if create_file:
        setFileContent(path, "data" + str(count))
    return path


def export_a_doc(portal):
    path = ensure_real_dir_file(True)
    doc = portal.add(path)
    logv("exported %s as %s" % (path, doc))

    lookup = portal.lookup(path)
    assert lookup == doc.id

    lookup_on_fuse = portal.lookup(doc.get_doc_path(None) + "/" + doc.filename)
    assert lookup_on_fuse == doc.id

    reused_doc = portal.add(path)
    assert doc is reused_doc

    not_reused_doc = portal.add(path, False)
    assert doc is not not_reused_doc

    # We should not be able to re-export a tmpfile
    tmppath = doc.get_doc_path(None) + "/tmpfile"
    setFileContent(tmppath, "tempdata")

    # Should not be able to add a tempfile on the fuse mount, or look it up
    assertRaises(GLib.Error, portal.add, tmppath)
    lookup = portal.lookup(tmppath)
    assert lookup == ""

    os.unlink(tmppath)


def export_a_named_doc(portal, create_file):
    path = ensure_real_dir_file(create_file)
    doc = portal.add_named(path)
    logv("exported (named) %s as %s" % (path, doc))

    if create_file:
        lookup = portal.lookup(path)
        assert lookup == doc.id

    reused_doc = portal.add_named(path)
    assert doc is reused_doc

    not_reused_doc = portal.add_named(path, False)
    assert doc is not not_reused_doc


def export_a_dir_doc(portal):
    (dir, count) = ensure_real_dir(False)
    doc = portal.add_dir(dir)
    logv("exported (dir) %s as %s" % (dir, doc))

    lookup = portal.lookup(dir)
    assert lookup == doc.id

    lookup_on_fuse = portal.lookup(doc.get_doc_path(None))
    assert lookup_on_fuse == doc.id

    # We should not be able to portal lookup a file in the dir doc
    subpath = doc.get_doc_path(None) + "/sub"
    setFileContent(subpath, "sub")
    doc = portal.lookup(subpath)
    assert doc == ""
    doc2 = portal.lookup(dir + "/sub")
    assert doc2 == ""

    # But we should be able to re-export the file
    reexported_doc = portal.add(subpath)
    reexported_docdir = reexported_doc.get_doc_path(None)
    assertFileHasContent(reexported_docdir + "/sub", "sub")
    portal.delete(reexported_doc.id)

    os.unlink(subpath)

    # And also re-export a directory
    os.mkdir(subpath)
    setFileContent(subpath + "/subfile", "subfile")
    reexported_doc = portal.add_dir(subpath)
    reexported_docdir = reexported_doc.get_doc_path(None)
    assertFileHasContent(reexported_docdir + "/subfile", "subfile")
    portal.delete(reexported_doc.id)

    os.unlink(subpath + "/subfile")
    os.rmdir(subpath)


def add_an_app(portal, num_docs):
    if num_docs == 0:
        return
    count = get_a_count("app")
    read_app = app_prefix + "read.App" + str(count)
    write_app = app_prefix + "write.App" + str(count)
    portal.ensure_app_id(read_app)
    portal.ensure_app_id(write_app)

    docs = portal.get_docs()
    ids = []
    for i in range(num_docs):
        if len(docs) == 0:
            continue
        indx = random.randint(0, len(docs) - 1)
        doc = docs[indx]
        del docs[indx]
        ids.append(doc.id)
        portal.grant_permissions(doc.id, read_app, ["read"])
        doc.apps.append(read_app)
        portal.grant_permissions(doc.id, write_app, ["read", "write"])
        doc.apps.append(write_app)
    logv("granted acces to %s and %s for %s" % (read_app, write_app, ids))

def file_transfer_portal_test():
    log("File transfer tests")
    ft_portal = FileTransferPortal()

    key = ft_portal.start_transfer()

    file1 = ensure_real_dir_file(True)
    file2 = ensure_real_dir_file(True)
    res = ft_portal.add_files(key, [file1, file2])

    res = ft_portal.retrieve_files(key)
    files = res[0]
    assert len(files) == 2
    # This is the same app, it's not sandboxed
    assert files[0] == file1
    assert files[1] == file2
    log("filetransfer tests ok")

    log("filetransfer dir")
    key = ft_portal.start_transfer()
    dir1 = ensure_real_dir(True)
    ft_portal.add_files(key, [file1, dir1[0], file2])
    res = ft_portal.retrieve_files(key)
    assert len(res[0]) == 3
    assert res[0][0] == file1
    assert res[0][1] == dir1[0]
    assert res[0][2] == file2
    log("filetransfer dir ok")

    log("filetransfer key")
    # Test that an invalid key is rejected
    key = ft_portal.start_transfer()
    assert key != "1234"
    assertRaisesGError("GDBus.Error:org.freedesktop.DBus.Error.AccessDenied", 9, ft_portal.add_files, "1234", [file1, file2])

    # Test stop transfer
    key = ft_portal.start_transfer()
    ft_portal.add_files(key, [file1, file2])
    ft_portal.stop_transfer(key)
    assertRaisesGError("GDBus.Error:org.freedesktop.DBus.Error.AccessDenied", 9, ft_portal.retrieve_files, key)
    assertRaisesGError("GDBus.Error:org.freedesktop.DBus.Error.AccessDenied", 9, ft_portal.add_files, key, [file1, file2])

    # Test that we can't reuse an old key
    new_key = ft_portal.start_transfer()
    assertRaisesGError("GDBus.Error:org.freedesktop.DBus.Error.AccessDenied", 9, ft_portal.add_files, key, [file1, file2])
    res = ft_portal.add_files(new_key, [file1, file2])
    log("filetransfer key ok")

    log("File transfer tests ok")

try:
    log("Connecting to portal")
    doc_portal = DocPortal()

    log("Running fuse tests...")
    create_app_by_lookup(doc_portal)
    verify_fs_layout(doc_portal)

    log("Creating some docs")
    for i in range(10):
        export_a_doc(doc_portal)
        verify_fs_layout(doc_portal)

    log("Creating some named docs (existing)")
    for i in range(10):
        export_a_named_doc(doc_portal, True)
    verify_fs_layout(doc_portal)

    log("Creating some named docs (non-existing)")
    for i in range(10):
        export_a_named_doc(doc_portal, False)
    verify_fs_layout(doc_portal)

    log("Creating some dir docs")
    for i in range(10):
        export_a_dir_doc(doc_portal)
    verify_fs_layout(doc_portal)

    log("Creating some apps")
    for i in range(10):
        add_an_app(doc_portal, 6)
    verify_fs_layout(doc_portal)

    for i in range(args.iterations):
        log("Checking permissions, pass %d" % (i + 1))
        check_perms(doc_portal)
        verify_fs_layout(doc_portal)

    log("fuse tests ok")
    file_transfer_portal_test()

    sys.exit(0)
except Exception as e:
    log("fuse tests failed: %s" % e)
    sys.exit(1)
