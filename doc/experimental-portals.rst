..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Experimental Portals
====================

New portals are often developed in collaboration with both desktop developers
and app developers. In order to get feedback on the portal, it may be
beneficial to implement an experimental portal interface. These interfaces are
intended to be temporary and do not promise any stability guarantees:
developers should expect that these interfaces may change or become unavailable
at any time.

Interface Naming Conventions
----------------------------

Experimental portal frontends must follow the following conventions:

- The frontend object path must be
  ``/org/freedesktop/portal/desktop/experimental``. The backend object path
  should be the same.
- The interface name must follow the pattern
  ``org.freedesktop.portal.Name.X#``, where ``#`` is a number that increments
  on major changes to the experimental portal.
- When the portal is promoted to stable, the ``.X#`` suffix in the interface
  will be dropped, and the version property will be reset to ``1``.
- The ``version`` property is a number that increments with minor changes to
  the portal that are backwards compatible with previous client implementations
  under the same major interface version. Clients should abort if the major
  version ``X#`` does not match exactly or if the ``version``
  property is lower than what they expect.

Experimental Merge Requirements
-------------------------------

In order to be merged into the project as an experimental portal, experimental
frontend implementations must meet minimum stability requirements, including:

- The intent and design of the portal must be discussed with and accepted by
  one of the committers.
- The code must be reasonably correct so as not to cause security issues for
  other portals running in the same process.
- Code and commit message hygiene as documented in :doc:`pull-requests` must be
  followed.

Exposing Experimental Portals
-----------------------------

Like all other portals, experimental portals will not start unless a backend is
configured and discovered at startup, so application developers will have to
install and configure an appropriate backend in order for the frontend to
become available. No other mechanism is required for exposing the experimental
frontend to application developers.

Stable Promotion
----------------

In order to be promoted to stable, the experimental portal must:

- Go through final review by the committers, including frontend code and
  documentation for both the frontend and backend.
- Be implemented and merged into at least one major backend implementation.
- Drop the version suffix in the interface, reset the ``version`` property to
  ``1``, and export the interface on the standard
  ``/org/freedesktop/portal/desktop`` object path.
