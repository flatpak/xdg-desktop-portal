Reasons to Use Portals
======================

Using XDG Desktop Portal brings major advantages over writing desktop-specific
code for app developers:

* **Strict sandbox**: portals enable sandboxed apps to access a curated set of
  features from the desktop environment and the host system, without weakening
  the sandbox of the app.
* **Unified code**: by using portal APIs, apps only need to write a single
  desktop-agnostic code that runs on a variety of desktop environments. Portals
  can be used by sandboxed and unsandboxed apps alike. App developers are
  encouraged to use portal APIs even for unsandboxed apps.
* **Seamless integration**: portal backends provide cost-free integration with
  the desktop. For example, simply using the :ref:`File Chooser portal<org.freedesktop.portal.FileChooser>`
  will make apps use the file picker dialog native to the desktop environment
  they're running on.
* **Permission system**: portals can restrict access to system resources through
  a permission system. Permissions can be granted, revoked, and controlled
  individually by the end user. This gives end users more control over apps.

In addition to the reasons above, some desktop features are primarily - and
sometimes exclusively - available through portals. For example, the primary way
of capturing screens and windows on Wayland desktops is through the
:ref:`ScreenCast portal<org.freedesktop.portal.ScreenCast>`, and some desktop
environments don't even expose other means to capture the screen or windows.