API Reference
=============

Portal interfaces are available to sandboxed applications with the default
filtered session bus access of Flatpak.

Desktop portals appear under the bus name ``org.freedesktop.portal.Desktop``
and the object path ``/org/freedesktop/portal/desktop`` on the session bus,
unless specified otherwise.

Apps running on the host system have access to a special interface for
registering themselves with XDG Desktop Portal. Registering a host app with
XDG Desktop Portal overwrites the automatic detection based on the
`XDG cgroup pathname standardization for applications
<https://systemd.io/DESKTOP_ENVIRONMENTS/#xdg-standardization-for-applications>`_.
This might improve the user experience when the host app was launched in a way
that doesn't follow the standard. See
:doc:`org.freedesktop.host.portal.Registry <doc-org.freedesktop.host.portal.Registry>`

Disclaimer: The host app registry is expected to eventually be deprecated and
may be removed. Applications should gracefully handle interface or method no
longer being available to be forward compatible. App launchers, or apps
themselves, should place the app in a cgroup named according to specific naming
conventions. When the host app registry becomes deprecated, the details of the
replacement will be documented in :doc:`org.freedesktop.host.portal.Registry
<doc-org.freedesktop.host.portal.Registry>`.

.. toctree::
   :hidden:

   doc-org.freedesktop.host.portal.Registry.rst

All apps have access to the portals below:

.. toctree::
   :maxdepth: 1

   doc-org.freedesktop.portal.Account.rst
   doc-org.freedesktop.portal.Background.rst
   doc-org.freedesktop.portal.Camera.rst
   doc-org.freedesktop.portal.Clipboard.rst
   doc-org.freedesktop.portal.Documents.rst
   doc-org.freedesktop.portal.DynamicLauncher.rst
   doc-org.freedesktop.portal.Email.rst
   doc-org.freedesktop.portal.FileChooser.rst
   doc-org.freedesktop.portal.FileTransfer.rst
   doc-org.freedesktop.portal.GameMode.rst
   doc-org.freedesktop.portal.GlobalShortcuts.rst
   doc-org.freedesktop.portal.Inhibit.rst
   doc-org.freedesktop.portal.InputCapture.rst
   doc-org.freedesktop.portal.Location.rst
   doc-org.freedesktop.portal.MemoryMonitor.rst
   doc-org.freedesktop.portal.NetworkMonitor.rst
   doc-org.freedesktop.portal.Notification.rst
   doc-org.freedesktop.portal.OpenURI.rst
   doc-org.freedesktop.portal.PowerProfileMonitor.rst
   doc-org.freedesktop.portal.Print.rst
   doc-org.freedesktop.portal.ProxyResolver.rst
   doc-org.freedesktop.portal.Realtime.rst
   doc-org.freedesktop.portal.RemoteDesktop.rst
   doc-org.freedesktop.portal.Request.rst
   doc-org.freedesktop.portal.ScreenCast.rst
   doc-org.freedesktop.portal.Screenshot.rst
   doc-org.freedesktop.portal.Secret.rst
   doc-org.freedesktop.portal.Session.rst
   doc-org.freedesktop.portal.Settings.rst
   doc-org.freedesktop.portal.Speech.rst
   doc-org.freedesktop.portal.Trash.rst
   doc-org.freedesktop.portal.Usb.rst
   doc-org.freedesktop.portal.Wallpaper.rst
