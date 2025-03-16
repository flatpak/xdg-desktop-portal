# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from typing import Callable, Dict, Optional, NamedTuple
from gi.repository import GLib
import dbus
import dbusmock
import logging


def init_logger(name: str) -> logging.Logger:
    """
    Common logging setup for the impl.portal templates. Use as:

        >>> from tests.templates import init_logger
        >>> logger = init_logger(__name__)
        >>> logger.debug("foo")

    """
    logging.basicConfig(
        format="%(levelname).1s|%(name)s: %(message)s", level=logging.DEBUG
    )
    logger = logging.getLogger(f"templates.{name}")
    logger.setLevel(logging.DEBUG)
    return logger


logger = init_logger("utils")


class Response(NamedTuple):
    response: int
    results: Dict


class ImplRequest:
    """
    Implementation of an ``org.freedesktop.impl.portal.Request`` object exposed
    on the object path ``handle``.

    The dbus method implementations need to be invoked asynchronously and the
    async callbacks must be passed in ``cb_success`` and ``cb_error``.

    The request either waits until it is closed by x-d-p (``wait_for_close``) or
    responds to the request (``respond``).
    """

    def __init__(
        self,
        mock: dbusmock.DBusMockObject,
        busname: str,
        handle: str,
        logger: logging.Logger,
        cb_success: Callable,
        cb_error: Callable,
    ):
        self.mock = mock
        bus = mock.connection
        proxy = bus.get_object(busname, handle)
        self.mock_interface = dbus.Interface(proxy, dbusmock.MOCK_IFACE)
        self.handle = handle
        self.logger = logger
        self.cb_success = cb_success
        self.cb_error = cb_error

    def respond(
        self,
        response: Callable | Response,
        delay: int = 0,
        done_cb: Callable | None = None,
    ) -> None:
        def reply():
            nonlocal response
            res = None
            logger.debug(f"Request {self.handle}: trying to reply")
            if callable(response):
                try:
                    res = response()
                except Exception as e:
                    logger.critical(
                        f"Request {self.handle}: failed getting response: {e}"
                    )
                    self.cb_error(e)
                    self._unexport()
                    return
            else:
                res = response

            assert res
            logger.debug(f"Request {self.handle}: replying {res}")

            self.cb_success(res.response, res.results)
            self._unexport()

            if done_cb:
                done_cb()

        self._export()

        if delay > 0:
            logger.debug(f"Request {self.handle}: scheduling delay of {delay}ms")
            GLib.timeout_add(delay, reply)
        else:
            reply()

    def wait_for_close(
        self,
        close_callback: Callable | None = None,
    ) -> None:
        def closed():
            logger.debug(f"Request {self.handle}: closed")

            self.mock.EmitSignal(
                "org.freedesktop.impl.portal.Mock",
                "RequestClosed",
                "s",
                (self.handle,),
            )

            if close_callback:
                try:
                    close_callback()
                except Exception as e:
                    logger.critical(
                        f"Request {self.handle}: failed running close callback: {e}"
                    )
                    self.cb_error(e)
                    self._unexport()
                    return

            response = Response(2, {})
            self.cb_success(response.response, response.results)
            self._unexport()

        def cb_methodcall(name, args):
            if name == "Close":
                closed()

        self._export()

        logger.debug(f"Request {self.handle}: waiting for x-d-p to call close")
        self.mock_interface.connect_to_signal("MethodCalled", cb_methodcall)

    def _export(self):
        # In the future we can pass a class extending
        # dbusmock.mockobject.DBusMockObject as mock_class to avoid going
        # through the mock MethodCalled signal
        self.mock.AddObject(
            path=self.handle,
            interface="org.freedesktop.impl.portal.Request",
            properties={},
            methods=[
                (
                    "Close",
                    "",
                    "",
                    "",
                )
            ],
        )

    def _unexport(self):
        self.mock.RemoveObject(self.handle)

    def __str__(self):
        return f"ImplRequest {self.handle}"


class ImplSession:
    """
    Implementation of a org.freedesktop.impl.portal.Session object. Do not
    instantiate this directly, instead use ``ImplSession.export()``. Typically
    like this:

        >>> s = ImplSession.export(mock, "org.freedesktop.impl.portal.desktop.Test", "/path/foo")

    Where the test or the backend implementation relies on the Closed() method
    of the ImplSession, provide a callback to be invoked.

        >>> r.export(close_callback=my_callback)

    Note that the latter only works if the test invokes methods
    asynchronously.

    .. attribute:: closed

        Set to True if the Close() method on the Session was invoked

    .. attribute:: handle

        The session's object path

    """

    def __init__(
        self,
        mock: dbusmock.DBusMockObject,
        busname: str,
        handle: str,
        app_id: str,
    ):
        self.mock = mock  # the main mock object
        self.handle = handle
        self.app_id = app_id
        self.closed = False
        self._close_callback: Optional[Callable] = None

        self.mock_object: Optional[dbusmock.DBusMockObject] = None

        bus = mock.connection
        proxy = bus.get_object(busname, handle)
        mock_interface = dbus.Interface(proxy, dbusmock.MOCK_IFACE)

        # Register for the Close() call on the impl.Session. If it gets
        # called, use the side-channel SessionClosed signal so we can notify
        # the test that the impl.Session was actually closed by the
        # xdg-desktop-portal
        def cb_methodcall(name, args):
            if name == "Close":
                self.closed = True
                logger.debug(f"Session.Close() on {self.handle}")
                if self._close_callback:
                    self._close_callback()
                self.mock.EmitSignal(
                    "org.freedesktop.impl.portal.Mock",
                    "SessionClosed",
                    "s",
                    (self.handle,),
                )
                self._unexport()

        mock_interface.connect_to_signal("MethodCalled", cb_methodcall)

    def export(
        self,
        close_callback: Optional[Callable] = None,
    ) -> "ImplSession":
        """
        Create the session on the bus. If ``close_callback`` is not None, that
        callback will be invoked in response to the Close() method called on
        this object.
        """
        self.mock.AddObject(
            path=self.handle,
            interface="org.freedesktop.impl.portal.Session",
            properties={},
            methods=[
                (
                    "Close",
                    "",
                    "",
                    "",
                )
            ],
        )
        # This is a bit awkward. We need our session's DBusMockObject for
        # EmitSignal of impl.portal.Session.Close. This is available in
        # dbusmock.get_object() since our template runs as part of the server.
        #
        # In theory, EmitSignal should work on self.mock_interface but
        # it doesn't and I can't figure out why.
        self.mock_object = dbusmock.get_object(self.handle)
        self._close_callback = close_callback
        return self

    def _unexport(self):
        self.mock.RemoveObject(path=self.handle)

    def close(self):
        """
        Send out Closed signal and remove this session from the bus.
        """
        assert self.mock_object is not None, "Session was never exported"
        logger.debug(f"Signal Session.Closed on {self.handle}")
        self.mock_object.EmitSignal(
            interface="org.freedesktop.impl.portal.Session",
            name="Closed",
            signature="",
            sigargs=(),
        )
        self.closed = True
        self._unexport()

    def __str__(self):
        return f"ImplSession {self.handle}"
