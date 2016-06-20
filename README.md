# xdg-desktop-portal

A portal frontend service for [Flatpak](http://www.flatpak.org) and possibly
other desktop containment frameworks.

xdg-desktop-portal works by exposing a series of D-Bus interfaces known as
_portals_ under a well-known name (org.freedesktop.portal.Desktop) and object
path (/org/freedesktop/portal/desktop).

The portal interfaces include APIs for file access, opening URIs, printing
and others.


