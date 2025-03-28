import os
from pathlib import Path

EXPORT_FILES_FLAG_EXPORT_DIR = 8


def path_from_null_term_bytes(bytes):
    path_bytes, rest = bytes.split(b"\x00")
    assert rest == b""
    return Path(os.fsdecode(path_bytes))


def get_mountpoint(documents_intf):
    mountpoint = documents_intf.GetMountPoint(byte_arrays=True)
    mountpoint = path_from_null_term_bytes(mountpoint)
    assert mountpoint.exists()
    return mountpoint


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
