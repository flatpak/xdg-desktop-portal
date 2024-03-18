#!/usr/bin/env python3
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
#

# Shared setup for portal tests. To test a portal, subclass TestPortal with
# your portal's name (e.g. TestEmail). This will auto-fill your portal
# name into some of the functions.
#
# Make sure the portal is listed in tests/portals/test.portal  and you have a
# dbusmock template for the impl.portal of your portal in tests/templates. See
# the dbusmock documentation for details on those templates.
#
# Environment variables:
#   G_TEST_BUILDDIR: override the path to the tests/ build
#                    directory (default: $PWD)
#   LIBEXECDIR: run xdg-desktop-portal from that dir
#   XDP_DBUS_MONITOR: if set, starts dbus_monitor on the custom bus, useful
#                     for debugging

from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib
from itertools import count
from typing import Any, Dict, Optional, NamedTuple
from pathlib import Path

import dbus
import dbus.proxies
import dbusmock
import fcntl
import logging
import os
import subprocess
import time

DBusGMainLoop(set_as_default=True)

# Anything that takes longer than 5s needs to fail
MAX_TIMEOUT = 5000

_counter = count()

ASV = Dict[str, Any]

logger = logging.getLogger("tests")


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
        self.error = None

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
            dbus_interface="org.freedesktop.impl.portal.Test",
        )

        logger.debug(f"Closing {self._closable} {self.objpath}")
        self._closable_interface.Close()

    def schedule_close(self, timeout_ms=300):
        """
        Schedule an automatic Close() on the given timeout in milliseconds.
        """
        assert 0 < timeout_ms < MAX_TIMEOUT
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
        GLib.timeout_add(MAX_TIMEOUT, self._mainloop.quit)

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


class PortalMock:
    """
    Parent class for portal tests.
    """

    def __init__(self, session_bus, portal_name: str, app_id: str = "org.example.App"):
        self.bus = session_bus
        self.portal_name = portal_name
        self.p_mock = None
        self.xdp = None
        self.portal_interfaces: Dict[str, dbus.Interface] = {}
        self.dbus_monitor = None
        self.app_id = app_id

    @property
    def interface_name(self) -> str:
        return f"org.freedesktop.portal.{self.portal_name}"

    @property
    def dbus_con(self):
        return self.bus.dbus_con

    def start_impl_portal(self, params=None, portal=None):
        """
        Start the impl.portal for the given portal name. If missing,
        the portal name is derived from the class name of the test, e.g.
        ``TestFoo`` will start ``org.freedesktop.impl.portal.Foo``.
        """
        portal = portal or self.portal_name
        self.p_mock, self.obj_portal = self.bus.spawn_server_template(
            template=f"tests/templates/{portal.lower()}.py",
            parameters=params,
            stdout=subprocess.PIPE,
        )
        flags = fcntl.fcntl(self.p_mock.stdout, fcntl.F_GETFL)
        fcntl.fcntl(self.p_mock.stdout, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        self.mock_interface = dbus.Interface(self.obj_portal, dbusmock.MOCK_IFACE)

        self.start_dbus_monitor()

    def add_template(self, portal, params: Dict[str, Any] = {}):
        """
        Add an additional template to the portal object
        """

        self.obj_portal.AddTemplate(
            f"tests/templates/{portal.lower()}.py",
            dbus.Dictionary(params, signature="sv"),
            dbus_interface=dbusmock.MOCK_IFACE,
        )

    def start_xdp(self):
        """
        Start the xdg-desktop-portal process
        """

        # This roughly resembles test-portals.c and glib's test behavior
        # but preferences in-tree testing by running pytest in meson's
        # project_build_root
        libexecdir = os.getenv("LIBEXECDIR")
        if libexecdir:
            xdp_path = Path(libexecdir) / "xdg-desktop-portal"
        else:
            xdp_path = (
                Path(os.getenv("G_TEST_BUILDDIR") or "tests")
                / ".."
                / "src"
                / "xdg-desktop-portal"
            )

        if not xdp_path.exists():
            raise FileNotFoundError(
                f"{xdp_path} does not exist, try running from meson build dir or setting G_TEST_BUILDDIR"
            )

        portal_dir = Path(os.getenv("G_TEST_BUILDDIR") or "tests") / "portals" / "test"
        if not portal_dir.exists():
            raise FileNotFoundError(
                f"{portal_dir} does not exist, try running from meson build dir or setting G_TEST_SRCDIR"
            )

        argv = [xdp_path]
        env = os.environ.copy()
        env["G_DEBUG"] = "fatal-criticals"
        env["XDG_DESKTOP_PORTAL_DIR"] = portal_dir
        env["XDG_CURRENT_DESKTOP"] = "test"
        env["XDG_DESKTOP_PORTAL_TEST_APP_ID"] = self.app_id

        xdp = subprocess.Popen(argv, env=env)

        for _ in range(50):
            if self.bus.dbus_con.name_has_owner("org.freedesktop.portal.Desktop"):
                break
            time.sleep(0.1)
        else:
            assert (
                False
            ), "Timeout while waiting for xdg-desktop-portal to claim the bus"

        self.xdp = xdp

    def start_dbus_monitor(self):
        if not os.getenv("XDP_DBUS_MONITOR"):
            return

        argv = ["dbus-monitor", "--session"]
        self.dbus_monitor = subprocess.Popen(argv)

    def tearDown(self):
        if self.dbus_monitor:
            self.dbus_monitor.terminate()
            self.dbus_monitor.wait()

        if self.xdp:
            self.xdp.terminate()
            self.xdp.wait()

        if self.p_mock:
            if self.p_mock.stdout:
                out = (self.p_mock.stdout.read() or b"").decode("utf-8")
                if out:
                    print(out)
                self.p_mock.stdout.close()
            self.p_mock.terminate()
            self.p_mock.wait()

    def get_xdp_dbus_object(self) -> dbus.proxies.ProxyObject:
        """
        Return the object that is the org.freedesktop.portal.Desktop proxy
        """
        try:
            return self._xdp_dbus_object
        except AttributeError:
            obj = self.bus.dbus_con.get_object(
                "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop"
            )
            # Useful for debugging:
            # print(obj.Introspect(dbus_interface="org.freedesktop.DBus.Introspectable"))
            assert obj
            self._xdp_dbus_object: dbus.proxies.ProxyObject = obj
            return self._xdp_dbus_object

    def get_dbus_interface(self, name=None) -> dbus.Interface:
        """
        Return the interface with the given name.

            >>> my_portal_intf = self.get_dbus_interface()
            >>> rd_portal_intf = self.get_dbus_interface("RemoteDesktop")
            >>> dbus_intf = self.get_dbus_interface("org.freedesktop.DBus.Introspectable")

        For portals, it's enough to specify the portal name (e.g. "InputCapture").
        If no name is provided, guess from the test class name.
        """
        name = name or self.interface_name
        if "." not in name:
            name = f"org.freedesktop.portal.{name}"

        try:
            intf = getattr(self, "portal_interfaces", {})[name]
        except KeyError:
            intf = dbus.Interface(self.get_xdp_dbus_object(), name)
            assert intf
            self.portal_interfaces[name] = intf
        return intf

    def create_request(self, intf_name: Optional[str] = None) -> Request:
        intf = self.get_dbus_interface(intf_name)
        return Request(self.dbus_con, intf)

    def check_version(self, expected_version):
        """
        Helper function to check for a portal's version. Use as:

            >>> class TestFoo(PortalMock):
            ...     def test_version(self):
            ...         self.check_version(2)
            >>>
        """
        properties_intf = self.get_dbus_interface("org.freedesktop.DBus.Properties")
        try:
            portal_version = properties_intf.Get(self.interface_name, "version")
            assert int(portal_version) == expected_version
        except dbus.exceptions.DBusException as e:
            logger.critical(e)
            assert e is None, str(e)
