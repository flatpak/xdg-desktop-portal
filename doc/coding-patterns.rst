..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

.. _coding-patterns:

Coding Patterns
===============

This page describes preferred patterns for new code and modernization of
existing code. Old code still uses some of the deprecated patterns described
here; new code and modifications should use the new patterns.

Variable Initialization
-----------------------

Only use trivial initialization in variable declarations. Move complex logic to
separate statements so it is not easy to miss.

.. code-block:: c

   g_autoptr(GTask) task = NULL;
   g_auto(GVariantBuilder) opt_builder =
     G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

   task = g_task_new (object, NULL, set_status_finished_cb, NULL);

Scope variables at their natural lifetime. Declare them in the narrowest scope
where they are needed so they are cleaned up automatically per iteration or
block.

.. code-block:: c

   for (size_t i = 0; i < items->len; i++)
     {
       g_autoptr(GError) error = NULL;
       g_autoptr(GVariant) result = NULL;

       result = process_item (g_ptr_array_index (items, i), &error);
       if (!result)
         {
           g_warning ("Failed: %s", error->message);
           continue;
         }

       use_result (result);
     }

Early Exits
-----------

Handle errors first and return early. Keep the success path at the leftmost
indentation level.

.. code-block:: c

   if (!validate_request (request))
     {
       g_set_error (error, ...);
       return FALSE;
     }

   if (!check_permissions (request))
     {
       g_set_error (error, ...);
       return FALSE;
     }

   return perform_operation (request, error);

Ownership and ``g_steal_pointer``
---------------------------------

Use ``g_autoptr``, ``g_autofree``, and ``g_autofd`` for all owned objects.
Make ownership transfers explicit with ``g_steal_pointer``.

.. code-block:: c

   g_autoptr(XdpSession) session = NULL;

   session = create_session ();
   configure_session (session);
   session_registry_add (registry, g_steal_pointer (&session));
   /* session is now NULL, registry owns it */

The same pattern applies when returning owned values or passing data to async
callbacks:

.. code-block:: c

   /* Returning ownership to the caller */
   return g_steal_pointer (&session);

.. code-block:: c

   /* Passing ownership to an async callback */
   xdp_dbus_impl_call_method (impl, ..., NULL,
                              callback,
                              g_steal_pointer (&request));

In the callback, take ownership immediately:

.. code-block:: c

   static void
   callback (GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
   {
     g_autoptr(XdpRequest) request = user_data;
     /* ... */
   }

D-Bus File Descriptor Handling
------------------------------

Portal methods receive file descriptors from clients via ``GUnixFDList``.
Helpers in ``xdp-utils.h`` handle index validation and safe copying:

``xdp_get_portal_call_fd``
  Validates the index, retrieves the FD, and returns an auto-closing descriptor.

  .. code-block:: c

     g_autofd int fd = xdp_get_portal_call_fd (fd_list, fd_id, error);
     if (fd < 0)
       return FALSE;

``xdp_copy_fd_to_lists``
  Copies an FD from one list to another, handling validation and cleanup.

  .. code-block:: c

     int dst_id;
     if (!xdp_copy_fd_to_lists (src_list, dst_list, src_id, &dst_id, error))
       return FALSE;

``xdp_is_fd_list_index_valid``
  Bounds-checks an index against an FD list. Called internally by the helpers
  above, but useful on its own when the FD is not needed immediately.

Error Reporting
---------------

Log full details internally for debugging, but return generic error messages to
clients. Internal paths, system state, and backend error details must not be
exposed to sandboxed applications.

.. code-block:: c

   if (!g_file_get_contents (path, &contents, NULL, &error))
     {
       g_warning ("Failed to open %s: %s", path, error->message);

       g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                              XDG_DESKTOP_PORTAL_ERROR,
                                              XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                              "Failed to open file");
       return G_DBUS_METHOD_INVOCATION_HANDLED;
     }

Always use the ``XDG_DESKTOP_PORTAL_ERROR`` domain for errors returned to
clients. Strip and log backend errors rather than forwarding them.

Options Validation
------------------

Use ``xdp_filter_options`` to validate and filter options dictionaries from
clients. Unknown keys are silently ignored; keys with the wrong type cause
the call to fail with an error.

.. code-block:: c

   static XdpOptionKey supported_options[] = {
     { "key1", G_VARIANT_TYPE_STRING, validate_key1 },
     { "key2", G_VARIANT_TYPE_BOOLEAN, NULL },
   };

   g_auto(GVariantBuilder) options_builder =
     G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

   if (!xdp_filter_options (options_in, &options_builder,
                            supported_options,
                            G_N_ELEMENTS (supported_options),
                            NULL, &error))
     {
       g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
       return G_DBUS_METHOD_INVOCATION_HANDLED;
     }

Asynchronous Code
-----------------

New code should use libdex fibers instead of threads or the traditional GLib
async pattern (``GTask`` with callbacks). Fibers let you write straight-line
code that suspends and resumes on the main loop without callbacks or explicit
state machines. Most existing portals still use ``GTask``; these are being
migrated over time.

Peer Disconnect Handling
------------------------

To react when a specific client disappears, connect to the
``XdpAppInfo::destroyed`` signal. This fires when the app that owns the
``XdpAppInfo`` disconnects from the bus.

``xdp_connection_track_peer_disconnect`` is a lower-level alternative that
tracks *all* clients on a connection. Prefer ``XdpAppInfo::destroyed`` when
cleaning up resources tied to a single app.

Invocation Return Value
-----------------------

Return ``G_DBUS_METHOD_INVOCATION_HANDLED`` from D-Bus method handlers instead
of bare ``TRUE``.

.. code-block:: c

   xdp_dbus_portal_complete_method (object, g_steal_pointer (&invocation));
   return G_DBUS_METHOD_INVOCATION_HANDLED;
