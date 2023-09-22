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
This mechanism is very similar to the freedesktop.org specification for
"Association between MIME types and applications" (mime-apps).

Desktop environments and OS vendors should provide a default configuration
for their chosen portal backends in
``/usr/share/xdg-desktop-portal/DESKTOP-portals.conf``, where ``DESKTOP``
is the desktop environment name as it would appear in the
``XDG_CURRENT_DESKTOP`` environment variable, after case-folding ASCII
upper case to lower case.
For example, KDE should provide ``/usr/share/xdg-desktop-portal/kde-portals.conf``.

Users can override those defaults, or provide configuration for an otherwise
unsupported desktop environment, by writing a file
``~/.config/xdg-desktop-portal/portals.conf``. Users of more than one
desktop environment can use desktop-specific filenames such as
``kde-portals.conf`` which will only be used in the appropriate desktop
environment.

Similarly, system administrators can provide a default configuration for
all users in ``/etc/xdg-desktop-portal/DESKTOP-portals.conf`` or
``/etc/xdg-desktop-portal/portals.conf``.

The following locations are searched for configuration, highest precedence
first:

- ``$XDG_CONFIG_HOME``, defaulting to ``~/.config``
- each directory in ``$XDG_CONFIG_DIRS``, defaulting to ``/etc/xdg``
- the build-time ``sysconfdir`` for xdg-desktop-portal, usually ``/etc``
- ``$XDG_DATA_HOME``, defaulting to ``~/.local/share``
  (searched only for consistency with other specifications, writing
  configuration here is not recommended)
- each directory in ``$XDG_DATA_DIRS``, defaulting to ``/usr/local/share:/usr/share``
- the build-time ``datadir`` for xdg-desktop-portal, usually ``/usr/share``

In each of those locations, for each desktop environment name listed in the
``XDG_CURRENT_DESKTOP`` environment variable, xdg-desktop-portal checks for
``xdg-desktop-portal/DESKTOP-portals.conf``, where ``DESKTOP`` is the
desktop environment name in lower-case. If a desktop-environment-specific
configuration file is not found, a non-desktop-specific file
``xdg-desktop-portal/portals.conf`` will be read.
For example, if ``XDG_CURRENT_DESKTOP`` is set to ``Budgie:GNOME``,
then xdg-desktop-portal will look for
``xdg-desktop-portal/budgie-portals.conf``,
``xdg-desktop-portal/gnome-portals.conf`` and
``xdg-desktop-portal/portals.conf`` in that order.

Only the first configuration file found is read, and lower-precedence
configuration files are ignored. All possible configuration files within
one directory are tried before moving on to the next directory, so for
example ``~/.config/xdg-desktop-portal/portals.conf`` is higher-precedence
than ``/usr/share/xdg-desktop-portal/kde-portals.conf``.

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

PORTAL LOADING
--------------

There are two types of interfaces: Exclusive interfaces, and unified
interfaces. Exclusive interfaces are provided by a single portal. Unified
interfaces are provided my multiple portals at once.

At the current time, the only unified interface is the Settings portal.

A portal is only loaded for an exclusive interface if configured, by either
being a member of the default list or the particular interface list.

A portal is loaded for a unified interface if the portal is a member of the
particular interface list, or by default if no configuration is provided for
that particular interface. As such, portals do not need to be in the default
list to be used for unified interfaces.

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

``XDG_CURRENT_DESKTOP``

  A colon-separated list of desktop environments, most specific first,
  used to choose a desktop-specific portal configuration.
  The default is an empty list.

``XDG_CONFIG_HOME``

  The per-user ``portals.conf`` file is located in this directory. The default
  is ``$HOME/.config``.

``XDG_CONFIG_DIRS``

  A colon-separated list of system configuration directories and secondary
  per-user configuration directories. The default is ``/etc/xdg``.

``XDG_DATA_HOME``

  A per-user data directory, searched for consistency with other
  specifications. The default is ``$HOME/.local/share``.

``XDG_DATA_DIRS``

  A colon-separated list of system data directories and secondary per-user
  data directories. The default is ``/usr/local/share:/usr/share``.

SEE ALSO
--------

- `XDG Base Directory Specification <https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html>`_
- `XDG Desktop Entry specification <https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html>`_
- `XDG Association between MIME type and applications specification <https://specifications.freedesktop.org/mime-apps-spec/mime-apps-spec-latest.html>`_
