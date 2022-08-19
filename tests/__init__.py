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
# Make sure you have a dbusmock template for the impl.portal of your portal in
# tests/templates. See the dbusmock documentation for details on those
# templates.
#
# Environment variables:
#   XDP_UNINSTALLED: run from $PWD (with an emulation of the glib test behavior)
#   LIBEXECDIR: run xdg-desktop-portal from that dir, required if
#               XDP_UNINSTALLED is unset
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


class Request:
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
        self.interface = interface
        self.response: Optional[Response] = None
        self.used = False
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error = None

        self._handle_token = f"request{next(_counter)}"
        self._mainloop: Optional[GLib.MainLoop] = None
        self._close_after_ms = 0
        self._closed = False
        self._impl_request_closed = False
        self._bus = bus

        def sanitize(name):
            return name.lstrip(":").replace(".", "_")

        sender_token = sanitize(bus.get_unique_name())
        self.handle = f"/org/freedesktop/portal/desktop/request/{sender_token}/{self._handle_token}"
        proxy = bus.get_object("org.freedesktop.portal.Desktop", self.handle)
        self.mock_interface = dbus.Interface(proxy, dbusmock.MOCK_IFACE)
        self._proxy = bus.get_object("org.freedesktop.portal.Desktop", self.handle)

        def cb_response(response: int, results: ASV) -> None:
            try:
                logger.debug(f"Response received on {self.handle}")
                assert not self._closed
                self.response = Response(response, results)
                if self._mainloop:
                    self._mainloop.quit()
            except Exception as e:
                self.error = e

        self.request_interface = dbus.Interface(proxy, "org.freedesktop.portal.Request")
        self.request_interface.connect_to_signal("Response", cb_response)

    @property
    def closed(self) -> bool:
        """
        True if both this request and the impl.Request were closed
        """
        return self._closed and self._impl_request_closed

    def close(self) -> None:
        signal_match = None

        def cb_impl_request_closed_by_portal(handle) -> None:
            if handle == self.handle:
                logger.debug(f"ImplRequest {self.handle} was closed")
                signal_match.remove()  # type: ignore
                self._impl_request_closed = True
                if self.closed and self._mainloop:
                    self._mainloop.quit()

        # See :class:`ImplRequest`, this signal is a side-channel for the
        # impl.portal template to notify us when the impl.Request was really
        # closed by the portal.
        signal_match = self._bus.add_signal_receiver(
            cb_impl_request_closed_by_portal,
            "RequestClosed",
            dbus_interface="org.freedesktop.impl.portal.Test",
        )

        logger.debug(f"Closing request {self.handle}")
        self._closed = True
        self.request_interface.Close()

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

        # Anything that takes longer than 5s needs to fail
        MAX_TIMEOUT = 5000

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

        if self._close_after_ms > 0:
            assert (
                self._close_after_ms < MAX_TIMEOUT
            ), "Cannot schedule Close() after the request timeout"
            GLib.timeout_add(self._close_after_ms, self.close)

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

    def schedule_close(self, timeout_ms=300):
        """
        Schedule an automatic Close() on the Request after the given timeout
        in milliseconds.
        """
        assert timeout_ms > 0
        self._close_after_ms = timeout_ms


class PortalTest(dbusmock.DBusTestCase):
    """
    Parent class for portal tests.
    """

    @classmethod
    def setUpClass(cls):
        logging.basicConfig(
            format="%(levelname).1s|%(name)s: %(message)s", level=logging.DEBUG
        )
        cls.PORTAL_NAME = cls.__name__.removeprefix("Test")
        cls.INTERFACE_NAME = f"org.freedesktop.portal.{cls.PORTAL_NAME}"
        cls.start_session_bus()
        cls.dbus_con = cls.get_dbus(system_bus=False)
        assert cls.dbus_con

    def setUp(self):
        self.p_mock = None
        self.xdp = None
        self.portal_interfaces = {}
        self.dbus_monitor = None

    def start_impl_portal(self, params=None, portal=None):
        """
        Start the impl.portal for the given portal name. If missing,
        the portal name is derived from the class name of the test, e.g.
        ``TestFoo`` will start ``org.freedesktop.impl.portal.Foo``.
        """
        portal = portal or self.PORTAL_NAME
        self.p_mock, self.obj_portal = self.spawn_server_template(
            template=f"tests/templates/{portal.lower()}.py",
            parameters=params,
            stdout=subprocess.PIPE,
        )
        flags = fcntl.fcntl(self.p_mock.stdout, fcntl.F_GETFL)
        fcntl.fcntl(self.p_mock.stdout, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        self.mock_interface = dbus.Interface(self.obj_portal, dbusmock.MOCK_IFACE)

        self.start_dbus_monitor()

    def start_xdp(self):
        """
        Start the xdg-desktop-portal process
        """

        # This roughly resembles test-portals.c and glib's test behavior
        if os.getenv("XDP_UNINSTALLED", None):
            builddir = Path(os.getenv("G_TEST_BUILDDIR") or "tests") / ".."
            # meson has x-d-p in a different build directory than the autotools build...
            xdp_path = builddir / "xdg-desktop-portal"
            if not xdp_path.exists():
                xdp_path = builddir / "src" / "xdg-desktop-portal"
        else:
            builddir = os.getenv("LIBEXECDIR")
            if not builddir:
                raise NotImplementedError("LIBEXECDIR is not set")
            xdp_path = Path(builddir) / "xdg-desktop-portal"

        distdir = os.getenv("G_TEST_SRCDIR") or "tests"
        portal_dir = Path(distdir) / "portals"

        argv = [xdp_path]
        env = os.environ.copy()
        env["G_DEBUG"] = "fatal-criticals"
        env["XDG_DESKTOP_PORTAL_DIR"] = portal_dir

        xdp = subprocess.Popen(argv, env=env)

        for _ in range(500):
            if self.dbus_con.name_has_owner("org.freedesktop.portal.Desktop"):
                break
            time.sleep(0.1)
        else:
            self.fail("Timeout while waiting for xdg-desktop-portal to claim the bus")

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
            obj = self.dbus_con.get_object(
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
        name = name or self.INTERFACE_NAME
        if "." not in name:
            name = f"org.freedesktop.portal.{name}"

        try:
            intf = self.portal_interfaces[name]
        except KeyError:
            intf = dbus.Interface(self.get_xdp_dbus_object(), name)
            assert intf
            self.portal_interfaces[name] = intf
        return intf

    def check_version(self, expected_version):
        """
        Helper function to check for a portal's version. Use as:

            >>> class TestFoo(PortalTest):
            ...     def test_version(self):
            ...         self.check_version(2)
            >>>
        """
        self.start_xdp()
        properties_intf = self.get_dbus_interface("org.freedesktop.DBus.Properties")
        try:
            portal_version = properties_intf.Get(self.INTERFACE_NAME, "version")
            assert int(portal_version) == expected_version
        except dbus.exceptions.DBusException as e:
            logger.critical(e);
            assert e is None, str(e)
