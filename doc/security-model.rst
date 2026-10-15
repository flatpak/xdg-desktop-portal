..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Security Model
==============

The user's desktop session — xdg-desktop-portal, backends, the compositor, and
other session services — is trusted. Sandboxed applications are not.

The portal frontend is the entry point that handles the trust boundary. It
receives D-Bus calls from sandboxed apps, validates all input, enforces
permissions, and keeps apps isolated from each other before forwarding requests
to the trusted backend.

::

   ┌──────────────────────────────────────────────────┐
   │ User's Desktop Session                 [TRUSTED] │
   │                                                  │
   │ Backend, compositor, permission store, etc.      │
   │                       ▲                          │
   │                       │                          │
   │ ┌─────────────────────┴────────────────────────┐ │
   │ │ xdg-desktop-portal (trust boundary)          │ │
   │ │ - Validates all input                        │ │
   │ │ - Enforces permissions                       │ │
   │ │ - Isolates apps from each other              │ │
   │ └──────────┬─────────────────────┬─────────────┘ │
   └────────────┼─────────────────────┼───────────────┘
                │                     │
   ═════════════╪═════════════════════╪════════════════
                │                     │
      ┌─────────┴────────┐  ┌────────┴─────────┐
      │ App A [UNTRUSTED]│  │ App B [UNTRUSTED]│
      └──────────────────┘  └──────────────────┘

Each application is a separate untrusted entity. Apps must not be able to access
each other's sessions, files, or data. A malicious app can send arbitrary D-Bus
messages with crafted parameters, supply malicious file descriptors, exploit
timing and concurrency, and probe for information through error messages.

Security Goals
--------------

- **Isolation:** the sandbox cannot be escaped through portal APIs
- **Least privilege:** apps get minimum necessary access
- **User consent:** sensitive operations require explicit user permission
- **App separation:** apps cannot access each other's sessions, files, or data
- **Host separation:** apps cannot access files or data on the host system
- **Fail-safe:** errors deny access rather than grant it
- **Information hiding:** internal state is not leaked through errors or timing

Input Validation
----------------

All input from sandboxed applications is untrusted. Every portal method must
validate its inputs before acting on them.

Use ``xdp_filter_options`` to validate options dictionaries. It drops unknown
keys and checks types, preventing clients from injecting unexpected options into
backend calls. See :ref:`coding-patterns` for details.

Use ``xdp_get_portal_call_fd`` to retrieve file descriptors from FD lists. It
validates the index before access, preventing out-of-bounds reads.

Do not use client-supplied paths directly. Files should be passed as open file
descriptors or document IDs, since passing an fd proves the app has access to
the file, and paths could subvert the sandbox's limited filesystem view.
Passing PIDs is similarly unreliable — sandboxed apps may be in their own PID
namespace and can lie about PIDs.

Information Disclosure
----------------------

Error messages returned to clients must not reveal internal paths, system
information, or backend error details. See the error reporting section in
:ref:`coding-patterns`.

Reporting Security Issues
-------------------------

Security vulnerabilities should be reported privately, not in public issues.
See `SECURITY.md <https://github.com/flatpak/xdg-desktop-portal/blob/main/SECURITY.md>`_
for details.
