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

- The interface name must follow the pattern
  ``org.freedesktop.portal.experimental.{Name}{N}``, where
  ``{N}`` is a number that increments on breaking changes to the
  experimental portal.
- As with stable portals, the ``version`` property is a number that increments
  with non-breaking changes to the portal.
- When the portal is promoted to stable, the ``.experimental`` prefix and
  ``{N}`` suffix in the interface name will be dropped, and the
  ``version`` property will be reset to ``1``.

Experimental Merge Requirements
-------------------------------

While experimental portals' features and implementations are subject to change
while in experimental status, in order to be merged into the project as an
experimental portal, they still must meet standard
:doc:`code hygiene <pull-requests>` and
:doc:`merge requirements <merge-requirements>`.

Exposing Experimental Portals
-----------------------------

Like all other portals, experimental portals will not start unless a backend is
configured and discovered at startup, so application developers will have to
install and configure an appropriate backend in order for the frontend to
become available.

Additionally, experimental portals are subject to a feature flag, expressed by
the ``XDG_CREDENTIAL_PORTAL_ENABLE_EXPERIMENTAL`` environment variable, which
should contain a comma-separated list of portal names, (e.g.
``XDG_CREDENTIAL_PORTAL_ENABLE_EXPERIMENTAL=Foo,Bar2``). xdg-desktop-portal will
check this at startup to determine whether to start the experimental portal.

Stable Promotion
----------------

In order to be promoted to stable, the experimental portal must:

- Go through final review by the committers, including frontend code and
  documentation for both the frontend and backend.
- Be implemented and merged into at least one major backend implementation.
- Drop the version suffix in the interface and reset the ``version`` property to
  ``1``.
