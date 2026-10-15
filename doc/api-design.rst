..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

API Design
==========

XDG Desktop Portal exposes two D-Bus APIs: the frontend API used by applications
and the backend API used by desktop environment components. Both must remain
backwards compatible.

The frontend/backend split exists for several reasons:

- Portal dialogs should be *native* to the session desktop (GTK for GNOME, Qt
  for KDE).
- Sandbox-facing D-Bus APIs should be hosted on a dedicated bus connection,
  since allowing an app to talk to a bus name implicitly allows it to talk to
  any other name owned by the same unique name. The frontend handles this.
- The frontend centralizes interaction with portal infrastructure (permission
  store, document store), freeing backends to focus on providing a user
  interface.
- The frontend handles argument validation, ensuring only valid requests reach
  the backend.

Portal APIs should be designed to hide behind existing library APIs where
possible, so that apps can use them transparently (e.g. the OpenFile portal
works as a backend for ``GtkFileChooserNative``).

Most portal methods use the :doc:`Request <requests>` pattern for asynchronous
operations. Long-lived interactions use :doc:`Sessions <sessions>`.

Compatibility Rules
-------------------

These rules apply to both the frontend and backend APIs.

Allowed changes:

- Adding new methods
- Adding optional keys to dictionaries
- Adding new properties or signals

Forbidden changes:

- Removing or renaming existing methods, properties, or signals
- Changing method signatures (add new parameters via dictionaries instead)
- Changing the semantics of existing dictionary keys (add a new key instead)

New optional parameters go in dictionaries (most commonly ``options``, but also
``results`` and others) with sensible defaults so old clients continue to work
unchanged.

Internal API
------------

C functions, headers, and structures within xdg-desktop-portal have no external
compatibility constraints and can change freely.

Version Discovery
-----------------

Both frontend and backend interfaces use version properties for feature
discovery.

.. code-block:: c

   /* Set the current version in the portal init */
   xdp_dbus_file_chooser_set_version (skeleton, 4);

.. code-block:: c

   /* Check backend version before using new features */
   unsigned int impl_version = xdp_dbus_impl_file_chooser_get_version (impl);

   if (impl_version >= 3)
     {
       g_variant_builder_add (&options, "{sv}",
                              "new_backend_feature",
                              g_variant_new_boolean (TRUE));
     }

Increment the version when adding new methods, dictionary keys, or changing
behavior. Do not increment for bug fixes or documentation changes.

Documenting API Changes
-----------------------

Every API change must be documented in the XML interface definition with type
signatures, semantics, default values, and the version that introduced each
dictionary key.

.. code-block:: xml

   <!--
     MethodName:
     @parent_window: Parent window identifier
     @options: Additional options
     @handle: Object path for the request

     Description of what this method does.

     Supported options:

     * ``option1`` (``s``)

       Description. Default: "value".

     * ``option2`` (``b``) (since version 2)

       Description. Default: false.

     This method was added in version 1.
     The ``option2`` option was added in version 2.
   -->
