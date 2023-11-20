Common Conventions
==================

Requests made via portal interfaces generally involve user interaction, and
dialogs that can stay open for a long time. Therefore portal APIs don't just use
async method calls (which time out after at most 25 seconds), but instead return
results via a Response signal on Request objects.

Portal APIs don't use properties very much. This is partially because we need to
be careful about the flow of information, and partially because it would be
unexpected to have a dialog appear when you just set a property. However, every
portal has at least one version property that specifies the maximum version
supported by xdg-desktop-portal.

Portal requests
---------------

The general flow of the portal API is that the application makes a portal
request, the portal replies to that method call with a handle (i.e. object path)
to a Request object that corresponds to the request. The object is exported on
the bus and stays alive as long as the user interaction lasts. When the user
interaction is over, the portal sends a Response signal back to the application
with the results from the interaction, if any.

To avoid a race condition between the caller subscribing to the signal after
receiving the reply for the method call and the signal getting emitted, a
convention for Request object paths has been established that allows the caller
to subscribe to the signal before making the method call.

Sessions
--------

Some portal requests are connected to each other and need to be used in
sequence. The pattern we use in such cases is a Session object. Just like
Requests, Sessions are represented by an object path, that is returned by the
initial CreateSession call of the respective portal. Subsequent calls take the
object path of the session they operate on as an argument.

Sessions can be ended from the application side by calling the Close() method on
the session. They can also be closed from the service side, in which case the
::Closed signal is emitted on the Session object to inform the application.

.. _window-identifiers:
Parent window identifiers
-------------------------

Most portals interact with the user by showing dialogs. These dialogs should
generally be placed on top of the application window that triggered them. To
arrange this, the compositor needs to know about the application window. Many
portal requests expect a "parent_window" string argument for this reason.

Under X11, the "parent_window" argument should have the form "x11:XID", where
XID is the XID of the application window in hexadecimal notation.

Under Wayland, it should have the form "wayland:HANDLE", where HANDLE is a
surface handle obtained with the `xdg_foreign
<https://github.com/wayland-project/wayland-protocols/blob/master/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml>`_
protocol.

For other windowing systems, or if you don't have a suitable handle, just pass
an empty string for "parent_window".

.. toctree::
   :maxdepth: 2
