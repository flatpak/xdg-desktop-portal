# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from typing import Callable, Dict, Optional, NamedTuple
import dbus
import dbusmock
import logging


def init_template_logger(name: str):
    """
    Common logging setup for the impl.portal templates. Use as:

        >>> from tests.templates import init_template_logger
        >>> logger = init_template_logger(__name__)
        >>> logger.debug("foo")

    """
    logging.basicConfig(
        format="%(levelname).1s|%(name)s: %(message)s", level=logging.DEBUG
    )
    logger = logging.getLogger(f"templates.{name}")
    logger.setLevel(logging.DEBUG)
    return logger


logger = init_template_logger("request")


class Response(NamedTuple):
    response: int
    results: Dict


class ImplRequest:
    """
    Implementation of a org.freedesktop.impl.portal.Request object. Typically
    this object needs to be merely exported:

        >>> r = ImplRequest(mock, "org.freedesktop.impl.portal.Test", handle)
        >>> r.export()

    Where the test or the backend implementation relies on the Closed() method
    of the ImplRequest, provide a callback to be invoked.

        >>> r.export(close_callback=my_callback)

    Note that the latter only works if the test invokes methods
    asynchronously.

    .. attribute:: closed

        Set to True if the Close() method on the Request was invoked

    """

    def __init__(self, mock: "dbusmock.DBusMockObject", busname: str, handle: str):
        self.mock = mock
        self.handle = handle
        self.closed = False
        self._close_callback: Optional[Callable] = None

        bus = mock.connection
        proxy = bus.get_object(busname, handle)
        mock_interface = dbus.Interface(proxy, dbusmock.MOCK_IFACE)

        # Register for the Close() call on the impl.Request. If it gets
        # called, use the side-channel RequestClosed signal so we can notify
        # the test that the impl.Request was actually closed by the
        # xdg-desktop-portal
        def cb_methodcall(name, args):
            if name == "Close":
                self.closed = True
                logger.debug(f"Close() on {self}")
                if self._close_callback:
                    self._close_callback()
                self.mock.EmitSignal(
                    "org.freedesktop.impl.portal.Test",
                    "RequestClosed",
                    "s",
                    (self.handle,),
                )
                self.mock.RemoveObject(self.handle)

        mock_interface.connect_to_signal("MethodCalled", cb_methodcall)

    def export(self, close_callback: Optional[Callable] = None):
        """
        Create the object on the bus. If close_callback is not None, that
        callback will be invoked in response to the Close() method called on
        this object.
        """
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
        self._close_callback = close_callback
        return self

    def __str__(self):
        return f"ImplRequest {self.handle}"
