# xdg-desktop-portal

A portal frontend service for [Flatpak](http://www.flatpak.org) and possibly
other desktop containment frameworks.

xdg-desktop-portal works by exposing a series of D-Bus interfaces known as
_portals_ under a well-known name (org.freedesktop.portal.Desktop) and object
path (/org/freedesktop/portal/desktop).

The portal interfaces include APIs for file access, opening URIs, printing
and others.

Documentation for the available D-Bus interfaces can be found
[here](http://flatpak.org/xdg-desktop-portal/portal-docs.html).

## Building xdg-desktop-portal

xdg-desktop-portal depends on GLib and Flatpak.
To build the documentation, you will need xmlto and the docbook stylesheets.

## Using portals

Flatpak grants sandboxed applications _talk_ access to names in the
org.freedesktop.portal.\* prefix. One possible way to use the portal APIs
is thus just to make D-Bus calls. For many of the portals, toolkits (e.g.
GTK+) are expected to support portals transparently if you use suitable
high-level APIs.

To actually use most portals, xdg-desktop-portal relies on a backend
that provides implementations of the org.freedesktop.impl.portal.\* interfaces.
One such backend is provided by [xdg-desktop-portal-gtk](http://github.com/flatpak/xdg-desktop-portal-gtk).
Another one is in development here: [xdg-desktop-portal-kde](https://github.com/KDE/xdg-desktop-portal-kde).
