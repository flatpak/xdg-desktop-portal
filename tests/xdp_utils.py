#!/usr/bin/env python3
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib, Gio
from itertools import count
from typing import Any, Dict, Optional, NamedTuple, Callable, List
from pathlib import Path
from enum import Enum
from dataclasses import dataclass, field
from urllib.parse import unquote, urlparse

import os
import dbus
import dbus.proxies
import dbusmock
import logging
import subprocess


DBusGMainLoop(set_as_default=True)

# Anything that takes longer than 5s needs to fail
DBUS_TIMEOUT = int(os.environ.get("XDP_DBUS_TIMEOUT", "5000"))

_counter = count()

ASV = Dict[str, Any]


def init_logger(name: str) -> logging.Logger:
    """
    Common logging setup for tests. Use as:

        >>> import tests.xdp_utils as xdp
        >>> logger = xdp.init_logger(__name__)
        >>> logger.debug("foo")

    """
    logging.basicConfig(
        format="%(levelname).1s|%(name)s: %(message)s", level=logging.DEBUG
    )
    logger = logging.getLogger(f"xdp.{name}")
    logger.setLevel(logging.DEBUG)
    return logger


logger = init_logger("utils")


def is_in_ci() -> bool:
    return os.environ.get("XDP_TEST_IN_CI") is not None


def is_in_container() -> bool:
    return is_in_ci() or (
        "container" in os.environ
        and (os.environ["container"] == "docker" or os.environ["container"] == "podman")
    )


def run_long_tests() -> bool:
    return os.environ.get("XDP_TEST_RUN_LONG") is not None


def check_program_success(cmd) -> bool:
    proc = subprocess.Popen(
        cmd, stdout=None, stderr=None, shell=True, universal_newlines=True
    )
    _ = proc.communicate()
    return proc.returncode == 0


def uri_same_file(uri1, uri2):
    orig = Path(unquote(urlparse(uri1).path))
    path = Path(unquote(urlparse(uri2).path))
    return orig.read_text() == path.read_text()


def uris_same_files(uris, uris_other):
    return all(
        uri_same_file(uri, uri_other) for uri, uri_other in zip(uris, uris_other)
    )


class FuseNotSupportedException(Exception):
    pass


def ensure_fuse_supported() -> None:
    if not check_program_success("fusermount3 --version"):
        raise FuseNotSupportedException("no fusermount3")

    if not check_program_success(
        "capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin'"
    ):
        raise FuseNotSupportedException(
            "No cap_sys_admin in bounding set, can't use FUSE"
        )

    if not check_program_success("[ -w /dev/fuse ]"):
        raise FuseNotSupportedException("no write access to /dev/fuse")

    if not check_program_success("[ -e /etc/mtab ]"):
        raise FuseNotSupportedException("no /etc/mtab")


def wait(ms: int):
    """
    Waits for the specified amount of milliseconds.
    """
    mainloop = GLib.MainLoop()
    GLib.timeout_add(ms, mainloop.quit)
    mainloop.run()


def wait_for(fn: Callable[[], bool]):
    """
    Waits and dispatches to mainloop until the function fn returns true. This is
    useful in combination with a lambda which captures a variable:

        my_var = False
        def callback():
            my_var = True
        do_something_later(callback)
        xdp.wait_for(lambda: my_var)
    """
    mainloop = GLib.MainLoop()
    while not fn():
        GLib.timeout_add(50, mainloop.quit)
        mainloop.run()


def get_permission_store_iface(bus: dbus.Bus) -> dbus.Interface:
    """
    Returns the dbus interface of the xdg-permission-store.
    """
    obj = bus.get_object(
        "org.freedesktop.impl.portal.PermissionStore",
        "/org/freedesktop/impl/portal/PermissionStore",
    )
    return dbus.Interface(obj, "org.freedesktop.impl.portal.PermissionStore")


def get_document_portal_iface(bus: dbus.Bus) -> dbus.Interface:
    """
    Returns the dbus interface of the xdg-document-portal.
    """
    obj = bus.get_object(
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
    )
    return dbus.Interface(obj, "org.freedesktop.portal.Documents")


def get_mock_iface(bus: dbus.Bus, bus_name: Optional[str] = None) -> dbus.Interface:
    """
    Returns the mock interface of the xdg-desktop-portal.
    """
    if not bus_name:
        bus_name = "org.freedesktop.impl.portal.Test"

    obj = bus.get_object(bus_name, "/org/freedesktop/portal/desktop")
    return dbus.Interface(obj, dbusmock.MOCK_IFACE)


def portal_interface_name(portal_name: str, domain: Optional[str] = None) -> str:
    """
    Returns the fully qualified interface for a portal name.
    """
    if domain:
        return f"org.freedesktop.{domain}.portal.{portal_name}"
    else:
        return f"org.freedesktop.portal.{portal_name}"


def get_portal_iface(
    bus: dbus.Bus, name: str, domain: Optional[str] = None
) -> dbus.Interface:
    """
    Returns the dbus interface for a portal name.
    """
    name = portal_interface_name(name, domain)
    return get_iface(bus, name)


def get_iface(bus: dbus.Bus, name: str) -> dbus.Interface:
    """
    Returns a named interface of the main portal object.
    """
    try:
        ifaces = bus._xdp_portal_ifaces
    except AttributeError:
        ifaces = bus._xdp_portal_ifaces = {}

    try:
        intf = ifaces[name]
    except KeyError:
        intf = dbus.Interface(get_xdp_dbus_object(bus), name)
        assert intf
        ifaces[name] = intf
    return intf


def get_xdp_dbus_object(bus: dbus.Bus) -> dbus.proxies.ProxyObject:
    """
    Returns the main portal object.
    """
    try:
        obj = getattr(bus, "_xdp_dbus_object")
    except AttributeError:
        obj = bus.get_object(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop"
        )
        assert obj
        bus._xdp_dbus_object = obj
    return obj


def check_version(bus: dbus.Bus, portal_name: str, expected_version: int):
    """
    Checks that the portal_name portal version is equal to expected_version.
    """
    properties_intf = dbus.Interface(
        get_xdp_dbus_object(bus), "org.freedesktop.DBus.Properties"
    )
    portal_iface_name = portal_interface_name(portal_name)
    try:
        portal_version = properties_intf.Get(portal_iface_name, "version")
        assert int(portal_version) == expected_version
    except dbus.exceptions.DBusException as e:
        logger.critical(e)
        assert e is None, str(e)


def desktop_files_path() -> Path:
    """Returns the default path for desktop files"""
    return Path(os.environ["XDG_DATA_HOME"]) / "applications"


class AppInfoKind(Enum):
    HOST = 1
    FLATPAK = 2
    SNAP = 3


@dataclass
class AppInfo:
    """
    Interacts with conftest.py via ensure_files and extend_env to make the
    portal frontend discover the requested XdpAppInfo for incoming connections.

    Testing code can use this class to construct a specific XdpAppInfo for the
    xdp_app_info fixture.
    """

    kind: AppInfoKind
    app_id: str
    desktop_file: str
    env: dict[str, str] = field(default_factory=dict)
    files: dict[Path, bytes] = field(default_factory=dict)

    def ensure_files(self) -> None:
        for path, content in self.files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    def extend_env(self, env: dict[str, str]) -> None:
        for key, val in self.env.items():
            env[key] = val

    def gapp_info(self) -> Gio.DesktopAppInfo:
        desktop_file_path = desktop_files_path() / self.desktop_file
        if not os.path.exists(desktop_file_path):
            return None

        return Gio.DesktopAppInfo.new_from_filename(str(desktop_file_path))

    @classmethod
    def new_host(
        cls,
        app_id: str,
        desktop_entry: bytes | None = None,
    ):
        kind = AppInfoKind.HOST
        desktop_file = f"{app_id}.desktop"
        env = {
            "XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND": "host",
            "XDG_DESKTOP_PORTAL_TEST_HOST_APPID": app_id,
        }
        files = {}

        if desktop_entry:
            files[desktop_files_path() / desktop_file] = desktop_entry

        return cls(
            kind=kind,
            app_id=app_id,
            desktop_file=desktop_file,
            env=env,
            files=files,
        )

    @classmethod
    def new_flatpak(
        cls,
        app_id: str,
        instance_id: str | None = None,
        usb_queries: str | None = None,
        desktop_entry: bytes | None = None,
        metadata: bytes | None = None,
    ):
        kind = AppInfoKind.FLATPAK
        desktop_file = f"{app_id}.desktop"
        env = {
            "XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND": "flatpak",
        }
        files = {}

        if not instance_id:
            instance_id = "1234567890"

        if not desktop_entry:
            desktop_entry = b"""
[Desktop Entry]
Version=1.0
Name=Example App
Exec=true %u
Type=Application
"""

        files[desktop_files_path() / desktop_file] = desktop_entry

        if not metadata:
            metadata_str = f"""
[Application]
name={app_id}
runtime=org.freedesktop.Platform/x86_64/23.08
sdk=org.freedesktop.Sdk/x86_64/23.08
command={app_id}

[Instance]
instance-id={instance_id}

[Context]
shared=network;ipc;
sockets=x11;wayland;pulseaudio;fallback-x11;
devices=dri;
"""
            metadata = metadata_str.encode("utf8")

        if usb_queries:
            metadata_usb_str = f"""
[USB Devices]
enumerable-devices={usb_queries}
"""
            metadata += metadata_usb_str.encode("utf8")

        metadata_path = Path(os.environ["TMPDIR"]) / "flatpak-metadata"

        files[metadata_path] = metadata
        env["XDG_DESKTOP_PORTAL_TEST_FLATPAK_METADATA"] = (
            metadata_path.absolute().as_posix()
        )

        return cls(
            kind=kind,
            app_id=app_id,
            desktop_file=desktop_file,
            env=env,
            files=files,
        )

    @classmethod
    def new_snap(
        cls,
        common_id: str,
        snap_name: str,
        app_name: str,
        desktop_entry: bytes | None = None,
        metadata: bytes | None = None,
    ):
        kind = AppInfoKind.SNAP
        app_id = f"snap.{snap_name}"
        desktop_file = f"{snap_name}_{app_name}.desktop"
        env = {
            "XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND": "snap",
        }
        files = {}

        if not desktop_entry:
            desktop_entry_str = f"""
[Desktop Entry]
Version=1.0
Name=Example App
Exec=true %u
Type=Application
X-SnapInstanceName={snap_name}
X-SnapAppName={app_name}
"""
            desktop_entry = desktop_entry_str.encode("UTF-8")

        files[desktop_files_path() / desktop_file] = desktop_entry

        if not metadata:
            metadata_str = f"""
[Snap Info]
InstanceName={snap_name}
AppName={app_name}
CommonID={common_id}
DesktopFile={desktop_file}
"""
            metadata = metadata_str.encode("UTF-8")

        metadata_path = Path(os.environ["TMPDIR"]) / "snap-metadata"

        files[metadata_path] = metadata
        env["XDG_DESKTOP_PORTAL_TEST_SNAP_METADATA"] = (
            metadata_path.absolute().as_posix()
        )

        return cls(
            kind=kind,
            app_id=app_id,
            desktop_file=desktop_file,
            env=env,
            files=files,
        )


class Response(NamedTuple):
    """
    Response as returned by a completed :class:`Request`
    """

    response: int
    results: ASV


class ResponseTimeout(Exception):
    """
    Exception raised by :meth:`Request.call` if the Request did not receive a
    Response in time.
    """

    pass


class Closable:
    """
    Parent class for both Session and Request. Both of these have a Close()
    method.
    """

    def __init__(self, bus: dbus.Bus, objpath: str):
        self.objpath = objpath
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error: Optional[Exception] = None

        self._mainloop: Optional[GLib.MainLoop] = None
        self._impl_closed = False
        self._bus = bus

        self._closable = type(self).__name__
        assert self._closable in ("Request", "Session")
        proxy = bus.get_object("org.freedesktop.portal.Desktop", objpath)
        self._closable_interface = dbus.Interface(
            proxy, f"org.freedesktop.portal.{self._closable}"
        )

    @property
    def bus(self) -> dbus.Bus:
        return self._bus

    @property
    def closed(self) -> bool:
        """
        True if the impl.portal was closed
        """
        return self._impl_closed

    def close(self) -> None:
        signal_match = None

        def cb_impl_closed_by_portal(handle) -> None:
            if handle == self.objpath:
                logger.debug(f"Impl{self._closable} {self.objpath} was closed")
                signal_match.remove()  # type: ignore
                self._impl_closed = True
                if self.closed and self._mainloop:
                    self._mainloop.quit()

        # See :class:`ImplRequest`, this signal is a side-channel for the
        # impl.portal template to notify us when the impl.Request was really
        # closed by the portal.
        signal_match = self._bus.add_signal_receiver(
            cb_impl_closed_by_portal,
            f"{self._closable}Closed",
            dbus_interface="org.freedesktop.impl.portal.Mock",
        )

        logger.debug(f"Closing {self._closable} {self.objpath}")
        self._closable_interface.Close()

    def schedule_close(self, timeout_ms=300):
        """
        Schedule an automatic Close() on the given timeout in milliseconds.
        """
        assert 0 < timeout_ms < DBUS_TIMEOUT
        GLib.timeout_add(timeout_ms, self.close)


class Request(Closable):
    """
    Helper class for executing methods that use Requests. This calls takes
    care of subscribing to the signals and invokes the method on the
    interface with the expected behaviors. A typical invocation is:

            >>> response = Request(connection, interface).call("Foo", bar="bar")
            >>> assert response.response == 0

    Requests can only be used once, to call a second method you must
    instantiate a new Request object.
    """

    def __init__(self, bus: dbus.Bus, interface: dbus.Interface):
        def sanitize(name):
            return name.lstrip(":").replace(".", "_")

        sender_token = sanitize(bus.get_unique_name())
        self._handle_token = f"request{next(_counter)}"
        self.handle = f"/org/freedesktop/portal/desktop/request/{sender_token}/{self._handle_token}"
        # The Closable
        super().__init__(bus, self.handle)

        self.interface = interface
        self.response: Optional[Response] = None
        self.used = False
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error: Optional[Exception] = None

        proxy = bus.get_object("org.freedesktop.portal.Desktop", self.handle)
        self.mock_interface = dbus.Interface(proxy, dbusmock.MOCK_IFACE)
        self._proxy = bus.get_object("org.freedesktop.portal.Desktop", self.handle)

        def cb_response(response: int, results: ASV) -> None:
            try:
                logger.debug(f"Response received on {self.handle}")
                assert self.response is None
                self.response = Response(response, results)
                if self._mainloop:
                    self._mainloop.quit()
            except Exception as e:
                self.error = e

        self.request_interface = dbus.Interface(proxy, "org.freedesktop.portal.Request")
        self.request_interface.connect_to_signal("Response", cb_response)

    @property
    def handle_token(self) -> dbus.String:
        """
        Returns the dbus-ready handle_token, ready to be put into the options
        """
        return dbus.String(self._handle_token, variant_level=1)

    def call(self, methodname: str, **kwargs) -> Optional[Response]:
        """
        Semi-synchronously call method ``methodname`` on the interface given
        in the Request's constructor. The kwargs must be specified in the
        order the DBus method takes them but the handle_token is automatically
        filled in.

            >>> response = Request(connection, interface).call("Foo", bar="bar")
            >>> if response.response != 0:
            ...     print("some error occured")

        The DBus call itself is asynchronous (required for signals to work)
        but this method does not return until the Response is received, the
        Request is closed or an error occurs. If the Request is closed, the
        Response is None.

        If the "reply_handler" and "error_handler" keywords are present, those
        callbacks are called just like they would be as dbus.service.ProxyObject.
        """
        assert not self.used
        self.used = True

        # Make sure options exists and has the handle_token set
        try:
            options = kwargs["options"]
        except KeyError:
            options = dbus.Dictionary({}, signature="sv")

        if "handle_token" not in options:
            options["handle_token"] = self.handle_token

        # Anything that takes longer than 5s needs to fail
        self._mainloop = GLib.MainLoop()
        GLib.timeout_add(DBUS_TIMEOUT, self._mainloop.quit)

        method = getattr(self.interface, methodname)
        assert method

        reply_handler = kwargs.pop("reply_handler", None)
        error_handler = kwargs.pop("error_handler", None)

        # Handle the normal method reply which returns is the Request object
        # path. We don't exit the mainloop here, we're waiting for either the
        # Response signal on the Request itself or the Close() handling
        def reply_cb(handle):
            try:
                logger.debug(f"Reply to {methodname} with {self.handle}")
                assert handle == self.handle

                if reply_handler:
                    reply_handler(handle)
            except Exception as e:
                self.error = e

        # Handle any exceptions during the actual method call (not the Request
        # handling itself). Can exit the mainloop if that happens
        def error_cb(error):
            try:
                logger.debug(f"Error after {methodname} with {error}")
                if error_handler:
                    error_handler(error)
                self.error = error
            except Exception as e:
                self.error = e
            finally:
                if self._mainloop:
                    self._mainloop.quit()

        # Method is invoked async, otherwise we can't mix and match signals
        # and other calls. It's still sync as seen by the caller in that we
        # have a mainloop that waits for us to finish though.
        method(
            *list(kwargs.values()),
            reply_handler=reply_cb,
            error_handler=error_cb,
        )

        self._mainloop.run()

        if self.error:
            raise self.error
        elif not self.closed and self.response is None:
            raise ResponseTimeout(f"Timed out waiting for response from {methodname}")

        return self.response


class Session(Closable):
    """
    Helper class for a Session created by a portal. This class takes care of
    subscribing to the `Closed` signals. A typical invocation is:

        >>> response = Request(connection, interface).call("CreateSession")
        >>> session = Session.from_response(response)
        # Now run the main loop and do other stuff
        # Check if the session was closed
        >>> if session.closed:
        ...    pass
        # or close the session explicitly
        >>> session.close()  # to close the session or
    """

    def __init__(self, bus: dbus.Bus, handle: str):
        assert handle
        super().__init__(bus, handle)

        self.handle = handle
        self.details = None
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error = None
        self._closed_sig_received = False

        def cb_closed(details: ASV) -> None:
            try:
                logger.debug(f"Session.Closed received on {self.handle}")
                assert not self._closed_sig_received
                self._closed_sig_received = True
                self.details = details
                if self._mainloop:
                    self._mainloop.quit()
            except Exception as e:
                self.error = e

        proxy = bus.get_object("org.freedesktop.portal.Desktop", handle)
        self.session_interface = dbus.Interface(proxy, "org.freedesktop.portal.Session")
        self.session_interface.connect_to_signal("Closed", cb_closed)

    @property
    def closed(self):
        """
        Returns True if the session was closed by the backend
        """
        return self._closed_sig_received or super().closed

    @classmethod
    def from_response(cls, bus: dbus.Bus, response: Response) -> "Session":
        return cls(bus, response.results["session_handle"])


class GDBusIfaceSignal:
    """
    Helper class which represents a connected signal on a GDBusIface and can be
    used to disconnect from the signal.
    """

    def __init__(self, signal_id: int, proxy: Gio.DBusProxy):
        self.signal_id = signal_id
        self.proxy = proxy

    def disconnect(self):
        """
        Disconnects the signal
        """
        self.proxy.disconnect(self.signal_id)


class GDBusIface:
    """
    Helper class for calling dbus interfaces with complex arguments.
    Usually you want to use python-dbus on the dbus_con fixture with
    get_portal_iface , get_mock_iface or get_iface. This is convenient but
    might not be sufficient for complex arguments or for asynchronously calling
    a method.
    """

    def __init__(self, bus: str, obj: str, iface: str):
        """
        Creates a GDBusIface for a specific bus, object and interface on the
        session bus.
        """
        address = Gio.dbus_address_get_for_bus_sync(Gio.BusType.SESSION, None)
        session_bus = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None,
            None,
        )
        assert session_bus
        self._proxy = Gio.DBusProxy.new_sync(
            session_bus,
            Gio.DBusProxyFlags.NONE,
            None,
            bus,
            obj,
            iface,
            None,
        )

    def _call(
        self, method_name: str, args_variant: GLib.Variant, fds: List[int] = []
    ) -> GLib.Variant:
        """
        Calls a method synchronously with the arguments passed in args_variant,
        passing the file descriptors specified in fds.
        Returns the result of the dbus call.
        """
        fdlist = Gio.UnixFDList.new()
        for fd in fds:
            fdlist.append(fd)

        return self._proxy.call_with_unix_fd_list_sync(
            method_name,
            args_variant,
            0,
            -1,
            fdlist,
            None,
        )

    def _call_async(
        self,
        method_name: str,
        args_variant: GLib.Variant,
        fds: List[int] = [],
        cb: Optional[Callable[[GLib.Variant], None]] = None,
    ) -> None:
        """
        Calls a method asynchronously with the arguments passed in args_variant,
        passing the file descriptors specified in fds.
        Invokes the callback cb when the call finished.
        """
        fdlist = Gio.UnixFDList.new()
        for fd in fds:
            fdlist.append(fd)

        def internal_cb(s, res, _):
            res = s.call_finish(res)
            if cb:
                cb(res)

        self._proxy.call_with_unix_fd_list(
            method_name,
            args_variant,
            0,
            -1,
            fdlist,
            None,
            internal_cb,
            None,
        )

    def connect_to_signal(
        self, name: str, cb: Callable[[GLib.Variant], None]
    ) -> GDBusIfaceSignal:
        """
        Connects to the dbus signal name to the callback cb. Returns an object
        representing the connection which can be used to disconnect it again.
        """

        def internal_cb(proxy, sender_name, signal_name, parameters):
            if signal_name != name:
                return
            cb(parameters)

        signal_id = self._proxy.connect("g-signal", internal_cb)
        return GDBusIfaceSignal(signal_id, self._proxy)
