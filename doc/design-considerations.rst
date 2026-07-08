..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Design Considerations
=====================

There are several reasons for the frontend/backend separation of the portal
code:

- We want to have *native* portal dialogs that match the session desktop (i.e.
  GTK dialogs for GNOME, Qt dialogs for KDE)
- One of the limitations of the D-Bus proxying in flatpak is that allowing a
  sandboxed app to talk to a name implicitly also allows it to talk to any other
  name owned by the same unique name. Therefore, sandbox-facing D-Bus APIs
  should generally be hosted on a dedicated bus connection. For portals, the
  frontend takes care of this for us.
- The frontend can handle all the interaction with *portal infrastructure*, such
  as the permission store and the document store, freeing the backends to focus
  on just providing a user interface.
- The frontend can also handle argument validation, and be strict about only
  letting valid requests through to the backend.

The portal apis are all following the pattern of an initial method call, whose
response returns an object handle for an ``org.freedesktop.portal.Request``
object that represents the portal interaction. The end of the interaction is
done via a ``Response`` signal that gets emitted on that object. This pattern
was chosen over a simple method call with return, since portal APIs are expected
to show dialogs and interact with the user, which may well take longer than the
maximum method call timeout of D-Bus. Another advantage is that the caller can
cancel an ongoing interaction by calling the ``Cancel`` method on the request
object.

One consideration for deciding the shape of portal APIs is that we want them to
'hide' behind existing library APIs where possible, to make it as easy as
possible to have apps use them *transparently*. For example, the OpenFile portal
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