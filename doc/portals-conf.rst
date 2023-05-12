.. _portals.conf(5):

============
portals.conf
============

--------------------------------
XDG desktop portal configuration
--------------------------------

DESCRIPTION
-----------

xdg-desktop-portal uses a configuration file to determine which portal backend
should be used to provide the implementation for the requested interface.

The configuration file can be found in the following locations:

- ``/etc/xdg-desktop-portal/portals.conf``, for site-wide configuration

- ``$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf``, for user-specific
  configuration

Additionally, every desktop environment can provide a portal configuration file
named ``DESKTOP-portals.conf``, where ``DESKTOP`` is the lowercase name also
used in the ``XDG_CURRENT_DESKTOP`` environment variable.

FILE FORMAT
-----------

The format of the portals configuration file is the same ``.ini`` format used by
systemd unit files or application desktop files.

``[preferred]``

  The main configuration group for preferred portals.

The following keys can be present in the ``preferred`` group:

``default`` *(string)*

  The default portal backend to use for every interface, unless the interface
  is listed explicitly.

``org.freedesktop.impl.portal.*`` *(string)*

  One of the valid portal interface implementations exposed by
  xdg-desktop-portal.

Each key in the group contains a semi-colon separated list of portal backend
implementation, to be searched for an implementation of the requested interface,
in the same order as specified in the configuration file. Additionally, the
special values ``none`` and ``*`` can be used:

``none``

  Do not provide a portal implementation for this interface.

``*``

  Use the first portal implementation found, in lexicographical order.

EXAMPLE
-------

::

  [preferred]
  # Use xdg-desktop-portal-gtk for every portal interface...
  default=gtk
  # ... except for the Screencast interface
  org.freedesktop.impl.portal.Screencast=gnome


ENVIRONMENT
-----------

``XDG_CONFIG_HOME``

  The per-user ``portals.conf`` file is located in this directory. The default
  is ``$HOME/.config``


SEE ALSO
--------

- `XDG Desktop Entry specification <https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html>`_
