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

To implement most portals, xdg-desktop-portal relies on a backend
that provides implementations of the org.freedesktop.impl.portal.\* interfaces.
One such backend is provided by [xdg-desktop-portal-gtk](http://github.com/flatpak/xdg-desktop-portal-gtk).
Another one is in development here: [xdg-desktop-portal-kde](https://github.com/KDE/xdg-desktop-portal-kde).

## Design considerations

There are several reasons for the frontend/backend separation of the portal
code:
- We want to have _native_ portal dialogs that match the session desktop (i.e. GTK+ dialogs
   for GNOME, Qt dialogs for KDE)
- One of the limitations of the D-Bus proxying in flatpak is that allowing a sandboxed app
  to talk to a name implicitly also allows it to talk to any other name owned by the same
  unique name. Therefore, sandbox-facing D-Bus apis should generally be hosted on a
  dedicated bus connection. For portals, the frontend takes care of this for us.
- The frontend can handle all the interaction with _portal infrastructure_, such as the
  permission store and the document store, freeing the backends to focus on just providing
  a user interface.
- The frontend can also handle argument validation, and be strict about only letting valid
  requests through to the backend.

The portal apis are all following the pattern of an initial method call, whose response returns an
object handle for an _org.freedesktop.portal.Request_ object that represents the portal interaction.
The end of the interaction is done via a _Response_ signal that gets emitted on that object. This
pattern was chosen over a simple method call with return, since portal apis are expected to
show dialogs and interact with the user, which may well take longer than the maximum method
call timeout of D-Bus. Another advantage is that the caller can cancel an ongoing interaction
by calling the _Cancel_ method on the request object.

One consideration for deciding the shape of portal APIs is that we want them to 'hide' behind existing
library APIs where possible, to make it as easy as possible to have apps use them _transparently_. For
example, the OpenFile portal is working well as a backend for the GtkFileChooserNative API.
