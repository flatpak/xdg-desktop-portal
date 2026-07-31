# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import os
from pathlib import Path
from typing import Any

import dbus
from gi.repository import Gio, GLib

EXPORT_FILES_FLAG_EXPORT_DIR = 8


def path_from_null_term_bytes(bytes: bytes) -> Path:
    path_bytes, rest = bytes.split(b"\x00")
    assert rest == b""
    return Path(os.fsdecode(path_bytes))


def get_mountpoint(documents_intf: dbus.Interface) -> Path:
    mountpoint = documents_intf.GetMountPoint(byte_arrays=True)
    mountpoint = path_from_null_term_bytes(mountpoint)
    assert mountpoint.exists()
    return mountpoint


def export_file(
    documents_intf: dbus.Interface, file_path: Path, unique: bool = False
) -> dbus.String:
    assert file_path.exists()

    with open(file_path.absolute().as_posix(), "r") as file:
        doc_id = documents_intf.Add(file.fileno(), not unique, False)
        assert doc_id

    return doc_id


def export_file_named(
    documents_intf: dbus.Interface,
    folder_path: Path,
    name: str,
    unique: bool = False,
) -> dbus.String:
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


def export_files(
    documents_intf: dbus.Interface,
    file_paths: list[Path],
    perms: list[str],
    flags: int = 0,
    app_id: str = "",
) -> tuple[Any, ...]:
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

    return result


def write_bytes_atomic(file_path: Path, bytes: bytes) -> None:
    GLib.file_set_contents(file_path.absolute().as_posix(), bytes)


def write_bytes_trunc(file_path: Path, bytes: bytes) -> None:
    try:
        fd = os.open(
            file_path.absolute().as_posix(), os.O_RDWR | os.O_TRUNC | os.O_CREAT
        )
        os.write(fd, bytes)
    finally:
        os.close(fd)


def get_host_path_attr(path: Path) -> Path | None:
    xattr = "xattr::document-portal.host-path"
    file = Gio.file_new_for_path(path.absolute().as_posix())
    info = file.query_info(xattr, Gio.FileQueryInfoFlags.NONE)
    host_path = info.get_attribute_as_string(xattr)
    if not host_path:
        return None
    return Path(os.fsdecode(host_path))
