For App Developers
==================

XDG Desktop Portal is a session service that provides D-Bus interfaces for apps
to interact with the desktop.

Portal interfaces can be used by sandboxed and unsandboxed apps alike, but
sandboxed apps benefit the most since they don't need special permissions to use
portal APIs. XDG Desktop Portal safeguards many resources and features with a
user-controlled permission system.

The primary goal of portals is to expose common functionality and integration
with the desktop without requiring apps to write desktop-specific code, or
loosen their sandbox restrictions.

.. toctree::
   :maxdepth: 1

   reasons-to-use-portals
   api-reference
