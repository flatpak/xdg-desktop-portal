#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# Generic helper to call a portal method from inside bwrap.
#
# Usage:
#   bwrap ... -- python3 bwrap-portal-call.py \
#       --interface org.freedesktop.portal.OpenURI \
#       --method OpenFile --fd <path>
#
#   bwrap ... -- python3 bwrap-portal-call.py \
#       --interface org.freedesktop.portal.FileChooser \
#       --method OpenFile --current-folder <path>

import argparse
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib


def make_request(bus, interface, method, args, fd_list):
    handle_token = "mount_ns_test"
    sender = bus.get_unique_name().lstrip(":").replace(".", "_")
    handle = f"/org/freedesktop/portal/desktop/request/{sender}/{handle_token}"

    mainloop = GLib.MainLoop()
    response_code = [-1]

    def on_signal(conn, sender_name, object_path, iface, signal, params):
        if object_path == handle and signal == "Response":
            response_code[0] = params[0]
            mainloop.quit()

    bus.signal_subscribe(
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        handle,
        None,
        Gio.DBusSignalFlags.NONE,
        on_signal,
    )

    proxy = Gio.DBusProxy.new_sync(
        bus,
        Gio.DBusProxyFlags.DO_NOT_AUTO_START,
        None,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        interface,
        None,
    )

    options = args.get("options", {})
    options["handle_token"] = GLib.Variant("s", handle_token)

    if fd_list is not None:
        fd_idx = args["fd_idx"]
        proxy.call_with_unix_fd_list_sync(
            method,
            GLib.Variant("(sha{sv})", ("", fd_idx, options)),
            Gio.DBusCallFlags.NONE,
            10000,
            fd_list,
            None,
        )
    else:
        title = args.get("title", "Test")
        proxy.call_sync(
            method,
            GLib.Variant("(ssa{sv})", ("", title, options)),
            Gio.DBusCallFlags.NONE,
            10000,
            None,
        )

    GLib.timeout_add(10000, mainloop.quit)
    mainloop.run()

    return response_code[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", required=True)
    parser.add_argument("--method", required=True)
    parser.add_argument("--fd", help="Open this path as an fd and pass it")
    parser.add_argument("--current-folder", help="Set current_folder option")
    args = parser.parse_args()

    bus = Gio.bus_get_sync(Gio.BusType.SESSION)

    call_args: dict = {"options": {}}
    fd_list = None

    if args.fd:
        fd = os.open(args.fd, os.O_RDONLY)
        fd_list = Gio.UnixFDList.new()
        fd_idx = fd_list.append(fd)
        os.close(fd)
        call_args["fd_idx"] = fd_idx

    if args.current_folder:
        call_args["options"]["current_folder"] = GLib.Variant(
            "ay", args.current_folder.encode() + b"\x00"
        )

    response = make_request(bus, args.interface, args.method, call_args, fd_list)

    sys.exit(0 if response == 0 else 1)


if __name__ == "__main__":
    main()
