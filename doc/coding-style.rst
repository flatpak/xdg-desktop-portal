..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Coding Style
============

The coding style follows the
`GNOME Coding Style <https://developer.gnome.org/documentation/guidelines/programming/coding-style.html>`_.
This page only lists where xdg-desktop-portal diverges or adds conventions.

Types
-----

New code should use standard C types (``int``, ``char``, ``unsigned int``,
``uint32_t``, ``size_t``). Use GLib types only where the API requires them
(``gboolean``). Older code still uses ``gchar``, ``guint``, etc.

Comments
--------

Use ``/* */`` comments only, not ``//``.

Headers
-------

Use ``#pragma once`` instead of traditional include guards. Include order:

1. ``config.h`` (always first)
2. System headers
3. GLib/GIO headers
4. Local headers
