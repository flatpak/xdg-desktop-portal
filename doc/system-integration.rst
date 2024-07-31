System Integration
==================

D-Bus Activation Environment
----------------------------

XDG Desktop Portal and its portal backends are activatable D-Bus services.
This means that they inherit environment variables from the "activation
environment" maintained by either ``systemd --user`` (on systems that use
systemd) or ``dbus-daemon`` (on systems that do not). They do not inherit
environment variables from the GUI environment or from the shell, unless
some component of the overall system takes responsibility for taking the
necessary environment variables from the GUI environment and sending them
to ``systemd`` or ``dbus-daemon`` to be added to the activation environment.

In integrated desktop environments such as GNOME and KDE Plasma, and in
OS distributions with a high level of integration, this should be done
automatically by desktop environment or OS infrastructure.

Variables that might need to be propagated in this way include, but are
not limited to:

- ``DISPLAY``
- ``PATH``
- ``WAYLAND_DISPLAY``
- ``XAUTHORITY``
- ``XDG_CURRENT_DESKTOP``
- ``XDG_DATA_DIRS``

In environments that are assembled out of individual components by
the user, it is the user's responsibility to ensure that this system
integration has been done, for example by using
`dbus-update-activation-environment(1)
<https://dbus.freedesktop.org/doc/dbus-update-activation-environment.1.html>`_
or `systemctl --user import-environment VAR…
<https://www.freedesktop.org/software/systemd/man/latest/systemctl.html>`_.

Desktop Environment Requirements
--------------------------------

The display manager or GUI environment is responsible for setting
``XDG_CURRENT_DESKTOP`` to an appropriate value.

The GUI environment should provide a
:doc:`portal configuration file </configuration-file>` with a name based on its
``XDG_CURRENT_DESKTOP``, to select appropriate portal backends.

The GUI environment should arrange for its required portal backend or
backends to be installed as dependencies (possibly as optional
dependencies, if it is packaged in a loosely-coupled operating system).

In environments that are assembled out of individual components by
the user, it is the user's responsibility to ensure that this system
integration has been done.