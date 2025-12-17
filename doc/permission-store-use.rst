Permission Store Use By Portals
===============================

Some portals use the :ref:`org.freedesktop.impl.portal.PermissionStore` for
storing application-specific permissions to avoid user dialogs. As per the 
:ref:`org.freedesktop.impl.portal.PermissionStore` documentation, each 
permission requires the use of a Table Name and within that table a Resource ID.
For permissions used by the XDG Desktop Portal itself, the convention is that
both Table Name and Resource ID are the lowercase name of the portal.

Within that Resource ID, permissions are stored using the application's app-id
as key. Permissions apply to the respective application only unless specified
otherwise in the documentation.

Given three applications, a typical permission store could look like this:

.. code-block:: json

   {
      "org.example.App1": "yes",
      "com.example.App2": "no",
      "net.example.App3": "ask",
   }

The value of each permission is specific to each portal. See the below
documentation for details.

:ref:`org.freedesktop.portal.Background`
----------------------------------------

- **Table Name:** ``"background"``
- **Resource ID:** ``"background"`` 
- **Value Type:** ``"yes" | "no" | "ask"``

If unset or set to ``"ask"``, a dialog is presented to the user.
If set to ``"yes"``, the request is forwarded to the portal implementation.

:ref:`org.freedesktop.portal.Camera`
------------------------------------

- **Table Name:** ``"camera"``
- **Resource ID:** ``"camera"`` 
- **Value Type:** ``"yes" | "no" | "ask"``

If unset or set to ``"ask"``, a dialog is presented to the user.
If set to ``"yes"``, the request is forwarded to the portal implementation.


:ref:`org.freedesktop.portal.InputCapture`
------------------------------------------

- **Table Name:** ``"inputcapture"``
- **Resource ID:** ``"inputcapture"`` 
- **Value Type:** ``[clamp_mask, yes_mask, ask_mask]`` as decimal integer strings.

If set, the three values represent a mask that affect the ``capabilities`` in the
:ref:`org.freedesktop.portal.InputCapture` ``CreateSession`` request.
The values are used as follows:

- the first value (``clamp_mask``) is the mask that the requested capabilities
  will be clamped to (i.e. ``capabilities & mask``) before the request is
  forwarded to the portal implementation.

- the second value (``yes_mask``) is the mask that will be immediately
  permitted. If the clamped capabilities are a subset of this mask, no dialog
  is presented to the user and the request is immediately forwarded to the
  portal implementation.

- the third value (``ask_mask``) is the mask of capabilities that require
  interactive user permission. If *any* of the clamped capabilities fall within
  this mask, a dialog is presented to the user.

If the clamped capabilities fall outside the ``ask_mask`` and ``yes_mask``, the
request is denied.

If no value is set in the PermissionStore, the default behavior is to present
a dialog to the user.


:ref:`org.freedesktop.portal.Screenshot`
----------------------------------------

- **Table Name:** ``"screenshot"``
- **Resource ID:** ``"screenshot"`` 
- **Value Type:** ``"yes" | "no" | "ask"``

If unset or set to ``"ask"``, a dialog is presented to the user.
If set to ``"yes"``, the request is forwarded to the portal implementation.
