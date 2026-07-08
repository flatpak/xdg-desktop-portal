<!--
SPDX-License-Identifier: LGPL-2.1-or-later
SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
-->

[![Portals](doc/_static/readme.png)](https://flatpak.github.io/xdg-desktop-portal/)

# [XDG Desktop Portal](https://flatpak.github.io/xdg-desktop-portal/)

Portals allow [Flatpak](https://flatpak.org) apps, and other desktop containment
frameworks, to interact with the system in a secure and well defined way.

XDG Desktop Portal works by exposing a series of D-Bus interfaces known as
_portals_ to apps. Portals are designed to be usable by all apps, including
ones which are confined by sandboxes, such as Flatpak.

The portal interfaces include APIs for file access, opening
URIs, printing and others.

XDG Desktop Portal works together with desktop environment specific backends to
mediate access to resources and functionality in an integrated manner.

Visit the [website](https://flatpak.github.io/xdg-desktop-portal/) for more
information.
