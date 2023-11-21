![Portals](doc/website/assets/readme.png)

# XDG Desktop Portal

A portal frontend service for [Flatpak](http://www.flatpak.org) and other
desktop containment frameworks.

xdg-desktop-portal works by exposing a series of D-Bus interfaces known as
_portals_ under a well-known name (`org.freedesktop.portal.Desktop`) and object
path (`/org/freedesktop/portal/desktop`).

The portal interfaces include APIs for file access, opening URIs, printing
and others.

Documentation for the available D-Bus interfaces can be found
[here](https://flatpak.github.io/xdg-desktop-portal/docs/).

## Version Numbering

xdg-desktop-portal uses even minor version numbers for stable versions, and odd
minor version numbers for unstable versions. During an unstable version cycle,
portal APIs can make backward incompatible changes, meaning that applications
should only depend on APIs defined in stable xdg-desktop-portal versions in
production.

## Building xdg-desktop-portal

xdg-desktop-portal depends on GLib and Flatpak.
To build the documentation, you will need xmlto and the docbook stylesheets.
For more instructions, please read [CONTRIBUTING.md][contributing].

## Using Portals

Flatpak grants sandboxed applications _talk_ access to names in the
org.freedesktop.portal.\* prefix. One possible way to use the portal APIs
is thus just to make D-Bus calls. For many of the portals, toolkits (e.g.
GTK) are expected to support portals transparently if you use suitable
high-level APIs.

To implement most portals, xdg-desktop-portal relies on a backend
that provides implementations of the org.freedesktop.impl.portal.\* interfaces.

Here are some examples of available backends:

- GTK [xdg-desktop-portal-gtk](http://github.com/flatpak/xdg-desktop-portal-gtk)
- GNOME [xdg-desktop-portal-gnome](https://gitlab.gnome.org/GNOME/xdg-desktop-portal-gnome/)
- KDE [xdg-desktop-portal-kde](https://invent.kde.org/plasma/xdg-desktop-portal-kde)
- LXQt [xdg-desktop-portal-lxqt](https://github.com/lxqt/xdg-desktop-portal-lxqt)
- Pantheon (elementary OS) [xdg-desktop-portal-pantheon](https://github.com/elementary/portals)
- wlroots [xdg-desktop-portal-wlr](https://github.com/emersion/xdg-desktop-portal-wlr)
- Deepin [xdg-desktop-portal-dde](https://github.com/linuxdeepin/xdg-desktop-portal-dde)
- Xapp (Cinnamon, MATE, Xfce) [xdg-desktop-portal-xapp](https://github.com/linuxmint/xdg-desktop-portal-xapp)

## Design Considerations

There are several reasons for the frontend/backend separation of the portal
code:

- We want to have _native_ portal dialogs that match the session desktop (i.e.
  GTK dialogs for GNOME, Qt dialogs for KDE)
- One of the limitations of the D-Bus proxying in flatpak is that allowing a
  sandboxed app to talk to a name implicitly also allows it to talk to any other
  name owned by the same unique name. Therefore, sandbox-facing D-Bus APIs
  should generally be hosted on a dedicated bus connection. For portals, the
  frontend takes care of this for us.
- The frontend can handle all the interaction with _portal infrastructure_, such
  as the permission store and the document store, freeing the backends to focus
  on just providing a user interface.
- The frontend can also handle argument validation, and be strict about only
  letting valid requests through to the backend.

The portal apis are all following the pattern of an initial method call, whose
response returns an object handle for an _org.freedesktop.portal.Request_ object
that represents the portal interaction. The end of the interaction is done via a
_Response_ signal that gets emitted on that object. This pattern was chosen over
a simple method call with return, since portal APIs are expected to show dialogs
and interact with the user, which may well take longer than the maximum method
call timeout of D-Bus. Another advantage is that the caller can cancel an
ongoing interaction by calling the _Cancel_ method on the request object.

One consideration for deciding the shape of portal APIs is that we want them to
'hide' behind existing library APIs where possible, to make it as easy as
possible to have apps use them _transparently_. For example, the OpenFile portal
is working well as a backend for the GtkFileChooserNative API.

When it comes to files, we need to be careful to not let portal APIs subvert the
limited filesystem view that apps have in their sandbox. Therefore, files should
only be passed into portal APIs in one of two forms:

- As a document ID referring to a file that has been exported in the document
  portal
- As an open fd. The portal can work its way back to a file path from the fd,
  and passing an fd proves that the app inside the sandbox has access to the
  file to open it.

When it comes to processes, passing PIDs around is not useful in a sandboxed
world where apps are likely in their own PID namespace. And passing PIDs from
inside the sandbox is problematic, since the app can just lie.

[contributing]: https://github.com/flatpak/xdg-desktop-portal/blob/main/CONTRIBUTING.md
