Configuration File
==================

Portal backends are selected and can be configured by using one or more
configuration files.

Desktop systems may have multiple desktop environments and portal backends
installed in parallel, and automatically picking portal backends in this
situation has proven to be a challenge.

For this reason, XDG Desktop Portal uses a config-based matching system.
Usually, the configuration files are provided by the desktop environments
themselves, so that e.g. the GNOME-specific portal backends are picked in
GNOME sessions. No end user intervention is necessary in this case.

Here's an example of a config file distributed by GNOME:

.. code-block::

   [preferred]
   default=gnome;gtk;
   org.freedesktop.impl.portal.Access=gnome-shell;gtk;
   org.freedesktop.impl.portal.Secret=gnome-keyring;

This file specifies that, by default, the ``gnome`` and ``gtk`` portal backends
must be used. However, specifically for :doc:`org.freedesktop.impl.portal.Access
<doc-org.freedesktop.impl.portal.Access>`, the ``gnome-shell`` and ``gtk``
portal backends must be used; and for :doc:`org.freedesktop.impl.portal.Secret
<doc-org.freedesktop.impl.portal.Secret>`, the ``gnome-keyring`` portal
backend must be used.

You can read more about the config file syntax, install locations, and more, in
the portals.conf man page:

.. toctree::
   :maxdepth: 1

   portals.conf