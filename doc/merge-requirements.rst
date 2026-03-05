Merge Requirements
==================

XDG Desktop Portal pull requests have a number of requirements that need to be
fulfilled, before they can get merged. The requirements are different, depending
on what part of the project is being changed.

- Any change requires at least one review by one of the committers
- Non-trivial changes to the Portal frontend or backend API, configuration
  changes which impact desktop integration, and similar changes require an ACK
  from a representative of every listed desktop environment
- Changes to the XdpAppInfo subclasses, and changes to the XdpAppInfo class
  which affect the subclasses require an ACK from a representative of the
  respective project (the host subclass is exempt from this requirement)

Those rules should be understood as strong guidelines. If in doubt, we should
try to talk with each other on Matrix or via video chat if necessary.

XdpAppInfo subclass duties
--------------------------

New XdpAppInfo subclasses can get added, but require representatives to be
listed in this document. The representatives are expected to be responsive to
issues concerning the XdpAppInfo subclass. If that is not case, the subclass
will get deleted from the source code again.

Current committers and representatives
--------------------------------------

If you want to be added, or removed, or want someone else to be added, reach out
on Matrix or via email.

Committers:

- Adrian Vovk
- David Redondo
- Georges Basile Stavracas Neto
- Jonas Ådahl
- Matthias Clasen
- Peter Hutterer
- Sebastian Wick

Flatpak representatives:

- Alexander Larsson
- Matthias Clasen
- Sebastian Wick

Snap representatives:

- James Henstridge
- Marco Trevisan

GNOME representatives:

- Georges Basile Stavracas Neto
- Jonas Ådahl
- Matthias Clasen
- Sebastian Wick

KDE representatives:

- David Edmundson
- David Redondo

Linyaps representative:

- He YuMing
- Iceyer
- reddevillg

Specialists
-----------

Portals cover a lot of different specialized topics. Committers might not be
familiar with all of them, so it can make sense to defer to specialists in those
topics, or in general get feedback from them. The list here should help you to
find and contact those specialists:

Printing:

- Till Kamppeter (OpenPrinting, GNOME, @tillkamppeter)