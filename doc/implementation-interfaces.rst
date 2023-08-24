D-Bus Interfaces for Portal Implementations
===========================================

The backend interfaces are used by the portal frontend to carry out portal
requests. They are provided by a separate process (or processes), and are not
accessible to sandboxed applications.

The separation of the portal infrastructure into frontend and backend is a clean
way to provide suitable user interfaces that fit into different desktop
environments, while sharing the portal frontend.

The portal backends are focused on providing user interfaces and accessing
session- or host-specific APIs and resources. Details of interacting with the
containment infrastructure such as checking access, registering files in the
document portal, etc., are handled by the portal frontend. 

.. toctree::
   :maxdepth: 1

   doc-org.freedesktop.impl.portal.Access.rst
   doc-org.freedesktop.impl.portal.Account.rst
   doc-org.freedesktop.impl.portal.AppChooser.rst
   doc-org.freedesktop.impl.portal.Background.rst
   doc-org.freedesktop.impl.portal.Clipboard.rst
   doc-org.freedesktop.impl.portal.DynamicLauncher.rst
   doc-org.freedesktop.impl.portal.Email.rst
   doc-org.freedesktop.impl.portal.FileChooser.rst
   doc-org.freedesktop.impl.portal.GlobalShortcuts.rst
   doc-org.freedesktop.impl.portal.Inhibit.rst
   doc-org.freedesktop.impl.portal.InputCapture.rst
   doc-org.freedesktop.impl.portal.Lockdown.rst
   doc-org.freedesktop.impl.portal.Notification.rst
   doc-org.freedesktop.impl.portal.PermissionStore.rst
   doc-org.freedesktop.impl.portal.Print.rst
   doc-org.freedesktop.impl.portal.RemoteDesktop.rst
   doc-org.freedesktop.impl.portal.Request.rst
   doc-org.freedesktop.impl.portal.ScreenCast.rst
   doc-org.freedesktop.impl.portal.Screenshot.rst
   doc-org.freedesktop.impl.portal.Secret.rst
   doc-org.freedesktop.impl.portal.Session.rst
   doc-org.freedesktop.impl.portal.Settings.rst
   doc-org.freedesktop.impl.portal.Wallpaper.rst
