Window Identifiers
==================

Most portals interact with the user by showing dialogs. These dialogs should
generally be placed on top of the application window that triggered them. To
arrange this, the compositor needs to know about the application window. Many
portal requests expect a ``"parent_window"`` string argument for this reason.

Under X11, the ``"parent_window"`` argument should have the form ``x11:<XID>``,
where ``<XID>`` is the XID of the application window in hexadecimal notation.
For example, ``x11:1234``.

Under Wayland, it should have the form ``wayland:<HANDLE>``, where ``<HANDLE>``
is a surface handle obtained with the `xdg_foreign
<https://github.com/wayland-project/wayland-protocols/blob/master/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml>`_
protocol. For example, ``wayland:~12l9jdl.-a``.

For other windowing systems, or if you don't have a suitable handle, just pass
an empty string for ``"parent_window"``.