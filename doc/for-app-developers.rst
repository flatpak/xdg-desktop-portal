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
   :hidden:

   reasons-to-use-portals
   convenience-libraries
   api-reference

.. cssclass:: tiled-toc

*  .. image:: _static/img/tiles/Reasons-l.png
      :target: reasons-to-use-portals.html
      :class: only-light
   .. image:: _static/img/tiles/Reasons-d.png
      :target: reasons-to-use-portals.html
      :class: only-dark

   :doc:`Reasons to Use Portals </reasons-to-use-portals>`

*  .. image:: _static/img/tiles/Libraries-l.png
      :target: convenience-libraries.html
      :class: only-light
   .. image:: _static/img/tiles/Libraries-d.png
      :target: convenience-libraries.html
      :class: only-dark

   :doc:`Convenience Libraries </convenience-libraries>`

*  .. image:: _static/img/tiles/APIs-l.png
      :target: api-reference.html
      :class: only-light
   .. image:: _static/img/tiles/APIs-d.png
      :target: api-reference.html
      :class: only-dark

   :doc:`API Reference </api-reference>`